#include "rockclimb/RockClimbPass.h"
#include "rockclimb/DistributedCheckpointing.h"
#include "rockclimb/RockClimbContext.h"
#include "rockclimb/RockClimbInstrumenter.h"
#include "rockclimb/RockClimbOptimizer.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"

#include <fstream>
#include <sstream>

using namespace llvm;

// Extern declaration for shared energy-config option (defined in PassRegistry.cpp)
extern cl::opt<std::string> EnergyConfigOpt;

// Command line options for RockClimb pass
cl::opt<std::string> RockClimbConfigOpt(
    "rockclimb-config",
    cl::desc("Path to JSON configuration file for RockClimb pass"),
    cl::value_desc("filename"),
    cl::init(""));

namespace {

static cl::opt<bool> DistributedCkptOpt(
    "rockclimb-distributed-ckpt",
    cl::desc("Enable distributed checkpointing (default: true)"),
    cl::init(true));

static cl::opt<std::string> CheckFnOpt(
    "rockclimb-check-function",
    cl::desc("Name of voltage check function to insert"),
    cl::init("__rockclimb_check"));

static cl::opt<std::string> SaveRegFnOpt(
    "rockclimb-save-reg-function",
    cl::desc("Name of register save function to insert"),
    cl::init("__rockclimb_save_reg"));

static cl::opt<bool> MemoryCkptOpt(
    "rockclimb-memory-ckpt",
    cl::desc("Enable memory checkpointing (allocas and globals) in addition to registers"),
    cl::init(false));

static cl::opt<bool> LoopUnrollOpt(
    "rockclimb-loop-unroll",
    cl::desc("Enable loop unrolling optimization before partitioning (default: true)"),
    cl::init(true));

} // anonymous namespace

namespace checkpoint {

// Parse RockClimb-specific parameters from flat JSON config.
// All fields are required except memory_checkpointing and checkpoint_store_energy.
bool parseRockClimbParams(StringRef configPath, RockClimbParams &params) {
    std::ifstream file(configPath.str());
    if (!file.is_open()) {
        errs() << "Error: Cannot open RockClimb config file: "
               << configPath << "\n";
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    Expected<json::Value> parsed = json::parse(content);
    if (!parsed) {
        consumeError(parsed.takeError());
        errs() << "Error: JSON parse error in RockClimb config: "
               << configPath << "\n";
        return false;
    }

    json::Object *root = parsed->getAsObject();
    if (!root) {
        errs() << "Error: RockClimb config is not a JSON object: "
               << configPath << "\n";
        return false;
    }

    // All fields required - flat JSON (no rockclimb_parameters wrapper)
    auto E_input = root->getNumber("E_input");
    if (!E_input) {
        errs() << "Error: Missing required field 'E_input' in RockClimb config: "
               << configPath << "\n";
        return false;
    }
    auto N_reg = root->getInteger("N_reg");
    if (!N_reg) {
        errs() << "Error: Missing required field 'N_reg' in RockClimb config: "
               << configPath << "\n";
        return false;
    }
    auto E_restore_per_reg = root->getNumber("E_restore_per_reg");
    if (!E_restore_per_reg) {
        errs() << "Error: Missing required field 'E_restore_per_reg' in RockClimb config: "
               << configPath << "\n";
        return false;
    }
    auto distributed = root->getBoolean("distributed_checkpointing");
    if (!distributed) {
        errs() << "Error: Missing required field 'distributed_checkpointing' in RockClimb config: "
               << configPath << "\n";
        return false;
    }

    params.E_input = *E_input;
    params.N_reg = static_cast<unsigned>(*N_reg);
    params.E_restore_per_reg = *E_restore_per_reg;
    params.distributedCheckpointing = *distributed;

    auto memCkpt = root->getBoolean("memory_checkpointing");
    if (!memCkpt) {
        errs() << "Error: Missing required field 'memory_checkpointing' in RockClimb config: "
               << configPath << "\n";
        return false;
    }
    params.memoryCheckpointing = *memCkpt;

    // Optional: checkpoint_store_energy (defaults to 0)
    auto ckptStoreEnergy = root->getNumber("checkpoint_store_energy");
    if (ckptStoreEnergy) {
        params.checkpoint_store_energy = *ckptStoreEnergy;
    }

    return true;
}

/// Try to unroll loops whose body energy fits within E_safe.
/// Paper Section IV-C.a: maximize region size by unrolling short loops.
static bool tryUnrollLoops(Function &F, LoopInfo &LI, ScalarEvolution &SE,
                           DominatorTree &DT, AssumptionCache &AC,
                           EnergyEstimator &estimator, double E_safe) {
    bool changed = false;

    // Collect loops first (unrolling modifies LoopInfo)
    SmallVector<Loop *, 4> loops;
    for (Loop *L : LI) {
        // Collect all loops including nested (innermost first)
        SmallVector<Loop *, 4> worklist;
        worklist.push_back(L);
        while (!worklist.empty()) {
            Loop *curr = worklist.pop_back_val();
            // Process innermost loops first
            if (curr->getSubLoops().empty()) {
                loops.push_back(curr);
            }
            for (Loop *sub : *curr) {
                worklist.push_back(sub);
            }
        }
    }

    for (Loop *L : loops) {
        // Only unroll loops with known trip count
        unsigned tripCount = SE.getSmallConstantTripCount(L);
        if (tripCount == 0) continue;

        // Compute body energy: sum of all blocks in the loop
        double bodyEnergy = 0.0;
        for (BasicBlock *BB : L->getBlocks()) {
            bodyEnergy += estimator.estimate(*BB).cost;
        }

        if (bodyEnergy >= E_safe) continue;  // Loop body already too big

        // Calculate unroll factor: how many iterations fit in E_safe
        unsigned maxUnroll = static_cast<unsigned>(E_safe / bodyEnergy);
        if (maxUnroll <= 1) continue;

        unsigned unrollFactor = std::min(maxUnroll, tripCount);
        if (unrollFactor <= 1) continue;

        // Set up unroll options
        UnrollLoopOptions ULO;
        ULO.Count = unrollFactor;
        ULO.Force = true;
        ULO.Runtime = false;
        ULO.AllowExpensiveTripCount = false;
        ULO.UnrollRemainder = (unrollFactor == tripCount);
        ULO.ForgetAllSCEV = true;

        errs() << "  Unrolling loop at "
               << L->getHeader()->getName()
               << " (trip count: " << tripCount
               << ", body energy: " << bodyEnergy
               << ", factor: " << unrollFactor << ")\n";

        LoopUnrollResult res = UnrollLoop(L, ULO, &LI, &SE, &DT, &AC,
                                          /*TTI=*/nullptr,
                                          /*ORE=*/nullptr,
                                          /*PreserveLCSSA=*/true);
        if (res != LoopUnrollResult::Unmodified) {
            changed = true;
        }
    }

    return changed;
}

PreservedAnalyses RockClimbPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
    // Create RockClimb context with separate estimator and rockclimb configs
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto ctxResult = createRockClimbContext(F, LI,
                                            EnergyConfigOpt.getValue(),
                                            RockClimbConfigOpt.getValue());

    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // Override distributed checkpointing from command line if specified
    bool useDistributedCkpt = ctx.params.distributedCheckpointing;
    if (DistributedCkptOpt.getNumOccurrences() > 0) {
        useDistributedCkpt = DistributedCkptOpt;
    }

    // Override memory checkpointing from command line if specified
    bool useMemoryCkpt = ctx.params.memoryCheckpointing;
    if (MemoryCkptOpt.getNumOccurrences() > 0) {
        useMemoryCkpt = MemoryCkptOpt;
    }

    errs() << "=== RockClimb Pass on " << F.getName() << " ===\n";
    errs() << "  E_input: " << ctx.params.E_input << "\n";
    errs() << "  E_safe: " << ctx.E_safe << "\n";
    errs() << "  Distributed checkpointing: "
           << (useDistributedCkpt ? "enabled" : "disabled") << "\n";
    errs() << "  Memory checkpointing: "
           << (useMemoryCkpt ? "enabled" : "disabled") << "\n";
    if (ctx.params.checkpoint_store_energy > 0) {
        errs() << "  Checkpoint store energy: "
               << ctx.params.checkpoint_store_energy << "\n";
    }

    // Fix 5: Loop unrolling optimization (before partitioning)
    if (LoopUnrollOpt) {
        auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
        auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
        auto &AC = AM.getResult<AssumptionAnalysis>(F);

        if (tryUnrollLoops(F, LI, SE, DT, AC, *ctx.estimator, ctx.E_safe)) {
            errs() << "  Loop unrolling applied, rebuilding CFG...\n";
            // Rebuild CFG analysis after unrolling
            ctx.cfg = std::make_unique<CFGAnalysis>(F, LI, *ctx.estimator);
        }
    }

    // Check feasibility - blocks that exceed E_safe individually
    RockClimbOptimizer optimizer(*ctx.cfg, ctx.E_safe, LI, F,
                                 ctx.estimator.get());
    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "Error: The following blocks exceed E_safe:\n";
        for (const auto &block : infeasible) {
            errs() << "  " << block << " (cost: "
                   << ctx.cfg->getBlockInfo(block).energyCost
                   << ", E_safe: " << ctx.E_safe << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Fix 2: Iterative partition-then-adjust loop
    // Partition, analyze checkpoint stores, compute CkptCycles, re-partition
    RockClimbOptimizer::Result result;
    std::vector<CheckpointPoint> checkpointPoints;

    static const int MAX_REFINEMENT_ITERATIONS = 3;
    for (int iteration = 0; iteration <= MAX_REFINEMENT_ITERATIONS; ++iteration) {
        result = optimizer.optimize();
        if (!result.feasible) {
            errs() << "Region partitioning failed: " << result.errorMessage << "\n";
            return PreservedAnalyses::all();
        }

        // If no checkpoint store energy configured, no need to iterate
        if (ctx.params.checkpoint_store_energy <= 0 || !useDistributedCkpt) {
            break;
        }

        // Run distributed checkpointing analysis to find checkpoint points
        DistributedCheckpointing distCkptAnalysis(F, result.regions,
                                                   result.regionBoundaries);
        auto currentCkptPoints = distCkptAnalysis.analyze();

        // Compute CkptCycles per block: count checkpoints per block * store energy
        std::map<std::string, double> CkptCycles;
        for (const auto &ckpt : currentCkptPoints) {
            if (ckpt.afterInst) {
                BasicBlock *BB = ckpt.afterInst->getParent();
                std::string blockName = BB->getName().str();
                if (blockName.empty()) {
                    size_t idx = 0;
                    for (BasicBlock &B : F) {
                        if (&B == BB) {
                            blockName = "bb" + std::to_string(idx);
                            break;
                        }
                        ++idx;
                    }
                }
                CkptCycles[blockName] += ctx.params.checkpoint_store_energy;
            }
        }

        // Check if CkptCycles changed from last iteration
        // (convergence check)
        bool converged = true;
        auto prevExtra = optimizer.getInfeasibleBlocks(); // side-effect free check

        // Simple convergence: same number of boundaries as last iteration
        auto prevBoundaryCount = result.regionBoundaries.size();

        // Apply CkptCycles and re-partition
        optimizer.setExtraBlockCosts(CkptCycles);

        if (iteration < MAX_REFINEMENT_ITERATIONS) {
            auto newResult = optimizer.optimize();
            if (newResult.regionBoundaries.size() == prevBoundaryCount) {
                // Converged — use the result from this iteration
                result = newResult;
                checkpointPoints = currentCkptPoints;
                break;
            }
            result = newResult;
            checkpointPoints = currentCkptPoints;
        } else {
            // Max iterations reached, use current result
            checkpointPoints = currentCkptPoints;
        }
    }

    errs() << "\nRegion boundaries (" << result.regionBoundaries.size() << "):\n";
    for (const auto &boundary : result.regionBoundaries) {
        errs() << "  " << boundary << "\n";
    }

    errs() << "\nRegions (" << result.regions.size() << "):\n";
    for (const auto &region : result.regions) {
        errs() << "  Region starting at " << region.startBlock
               << " (energy: " << region.totalEnergy
               << ", blocks: " << region.blocks.size() << ")\n";
    }

    // Distributed checkpointing analysis (registers and optionally memory)
    MemoryCheckpointResult memCheckpoints;

    DistributedCheckpointing distCkpt(F, result.regions, result.regionBoundaries);

    if (useDistributedCkpt) {
        // Use checkpointPoints from iterative refinement if available,
        // otherwise compute fresh
        if (checkpointPoints.empty()) {
            checkpointPoints = distCkpt.analyze();
        }

        errs() << "\nDistributed register checkpoints (" << checkpointPoints.size() << "):\n";
        for (const auto &ckpt : checkpointPoints) {
            errs() << "  Reg " << ckpt.regId << " in region " << ckpt.regionName;
            if (ckpt.afterInst) {
                errs() << " after instruction: ";
                ckpt.afterInst->print(errs());
            }
            errs() << "\n";
        }
    }

    if (useMemoryCkpt) {
        memCheckpoints = distCkpt.analyzeMemory();

        errs() << "\nMemory checkpoints (" << memCheckpoints.checkpoints.size() << "):\n";
        for (const auto &ckpt : memCheckpoints.checkpoints) {
            errs() << "  " << ckpt.nvmSlotName << " at boundary " << ckpt.boundaryId
                   << " in region " << ckpt.regionName << "\n";
        }
    }

    // Convert boundaries to set for instrumenter
    std::set<std::string> boundarySet(result.regionBoundaries.begin(),
                                       result.regionBoundaries.end());

    // Skip entry block from voltage checks (first boundary is always entry)
    // Entry doesn't need a voltage check since execution just started
    if (!boundarySet.empty()) {
        boundarySet.erase(ctx.cfg->getEntryBlock());
    }

    // Instrument the function
    RockClimbInstrumenter instrumenter(*F.getParent(), CheckFnOpt, SaveRegFnOpt);
    unsigned count = instrumenter.instrumentFunction(
        F, boundarySet, checkpointPoints, useDistributedCkpt);

    // Memory checkpointing instrumentation
    unsigned memCkptCount = 0;
    if (useMemoryCkpt && !memCheckpoints.checkpoints.empty()) {
        // Instrument memory saves at boundaries
        memCkptCount = instrumenter.instrumentMemoryCheckpoints(
            F, memCheckpoints, result.regionBoundaries);

        // Build boundary block map for recovery targets
        std::map<unsigned, BasicBlock*> boundaryBlocks;
        for (size_t i = 0; i < result.regionBoundaries.size(); ++i) {
            for (BasicBlock &BB : F) {
                std::string blockName = BB.getName().str();
                if (blockName.empty()) {
                    // Unnamed block - compute name
                    size_t idx = 0;
                    for (BasicBlock &B : F) {
                        if (&B == &BB) {
                            blockName = "bb" + std::to_string(idx);
                            break;
                        }
                        ++idx;
                    }
                }
                if (blockName == result.regionBoundaries[i]) {
                    boundaryBlocks[static_cast<unsigned>(i)] = &BB;
                    break;
                }
            }
        }

        // Insert recovery dispatcher at function entry (restore logic is inlined)
        std::map<unsigned, Function*> emptyRestoreFns;  // Not used anymore
        instrumenter.insertRecoveryDispatcher(F, emptyRestoreFns, boundaryBlocks);

        errs() << "\nMemory checkpoint instrumentation:\n";
        errs() << "  Memory saves: " << memCkptCount << "\n";
        errs() << "  Recovery boundaries: " << boundaryBlocks.size() << "\n";
    }

    errs() << "\nInserted " << count << " instrumentation point(s)\n";
    if (memCkptCount > 0) {
        errs() << "Inserted " << memCkptCount << " memory checkpoint(s)\n";
    }

    // Print comparison metrics
    errs() << "\n=== RockClimb Metrics ===\n";
    errs() << "  Regions: " << result.regions.size() << "\n";
    errs() << "  Boundary checks: " << boundarySet.size() << "\n";
    errs() << "  Register checkpoints: " << checkpointPoints.size() << "\n";
    errs() << "  Memory checkpoints: " << memCheckpoints.checkpoints.size() << "\n";

    double totalRegionEnergy = 0;
    double maxRegionEnergy = 0;
    for (const auto &region : result.regions) {
        totalRegionEnergy += region.totalEnergy;
        maxRegionEnergy = std::max(maxRegionEnergy, region.totalEnergy);
    }
    if (!result.regions.empty()) {
        errs() << "  Avg region energy: "
               << (totalRegionEnergy / result.regions.size()) << "\n";
        errs() << "  Max region energy: " << maxRegionEnergy << "\n";
    }

    // We modified the IR
    return PreservedAnalyses::none();
}

} // namespace checkpoint

// Note: Plugin registration is in src/common/PassRegistry.cpp
// This file just provides the pass implementation
