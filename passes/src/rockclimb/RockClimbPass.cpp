#include "rockclimb/RockClimbPass.h"
#include "rockclimb/DistributedCheckpointing.h"
#include "rockclimb/RockClimbContext.h"
#include "rockclimb/RockClimbInstrumenter.h"
#include "rockclimb/RockClimbOptimizer.h"
#include "common/BlockUtils.h"


#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

#include <chrono>
#include <fstream>
#include <sstream>

#define DEBUG_TYPE "rockclimb"

using namespace llvm;

// Extern declaration for shared energy-config option (defined in PassRegistry.cpp)
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<bool> AddDebugMarkersOpt;

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

static cl::opt<bool> LoopUnrollOpt(
    "rockclimb-loop-unroll",
    cl::desc("Enable loop unrolling optimization before partitioning (default: true)"),
    cl::init(true));

} // anonymous namespace

namespace checkpoint {
namespace {

static bool resolveFeatureToggle(const cl::opt<bool> &opt,
                                 bool configDefault) {
    if (opt.getNumOccurrences() > 0) {
        return opt;
    }
    return configDefault;
}

static void printPassConfig(const Function &F,
                            const RockClimbContext &ctx,
                            bool useDistributedCkpt,
                            bool addDebugMarkers) {
    errs() << "=== RockClimb Pass on " << F.getName() << " ===\n";
    errs() << "  E_input: " << ctx.params.E_input << "\n";
    errs() << "  E_safe: " << ctx.E_safe << "\n";
    errs() << "  Distributed checkpointing: "
           << (useDistributedCkpt ? "enabled" : "disabled") << "\n";
    errs() << "  Debug markers: "
           << (addDebugMarkers ? "enabled" : "disabled") << "\n";
    if (ctx.params.checkpoint_store_energy > 0) {
        errs() << "  Checkpoint store energy: "
               << ctx.params.checkpoint_store_energy << "\n";
    }
}

static DenseMap<BasicBlock*, double> computeCheckpointStoreCycles(
    const std::vector<CheckpointPoint> &checkpointPoints,
    double storeEnergyPerCheckpoint) {
    DenseMap<BasicBlock*, double> checkpointStoreCycles;
    for (const auto &ckpt : checkpointPoints) {
        if (!ckpt.afterInst) {
            continue;
        }

        BasicBlock *BB = ckpt.afterInst->getParent();
        checkpointStoreCycles[BB] += storeEnergyPerCheckpoint;
    }
    return checkpointStoreCycles;
}

static void collectInnermostLoops(Loop *L, SmallVectorImpl<Loop *> &out) {
    if (L->getSubLoops().empty()) {
        out.push_back(L);
        return;
    }
    for (Loop *sub : *L) {
        collectInnermostLoops(sub, out);
    }
}

} // namespace

// Parse RockClimb-specific parameters from flat JSON config.
// checkpoint_store_energy and add_debug_markers are optional.
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
    auto reg_restore_energy = root->getNumber("reg_restore_energy");
    if (!reg_restore_energy) {
        errs() << "Error: Missing required field 'reg_restore_energy' in RockClimb config: "
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
    params.reg_restore_energy = *reg_restore_energy;
    params.distributedCheckpointing = *distributed;

    // Optional: checkpoint_store_energy (defaults to 0)
    auto ckptStoreEnergy = root->getNumber("checkpoint_store_energy");
    if (ckptStoreEnergy) {
        params.checkpoint_store_energy = *ckptStoreEnergy;
    }

    // Optional: add_debug_markers (defaults to false)
    params.addDebugMarkers = false;
    if (root->get("add_debug_markers")) {
        auto addDebugMarkers = root->getBoolean("add_debug_markers");
        if (!addDebugMarkers) {
            errs() << "Error: Field 'add_debug_markers' must be boolean in "
                   << "RockClimb config: " << configPath << "\n";
            return false;
        }
        params.addDebugMarkers = *addDebugMarkers;
    }

    return true;
}

/// Try to unroll loops whose body energy fits within E_safe.
/// Paper Section IV-C.a: maximize region size by unrolling short loops.
static bool tryUnrollLoops(Function &F, LoopInfo &LI, ScalarEvolution &SE,
                           DominatorTree &DT, AssumptionCache &AC,
                           EnergyEstimator &estimator, double E_safe) {
    struct UnrollStats {
        unsigned loopsSeen = 0;
        unsigned skippedNotSimplify = 0;
        unsigned skippedUnknownTrip = 0;
        unsigned skippedBodyTooLarge = 0;
        unsigned skippedFactorTooSmall = 0;
        unsigned skippedLLVMRejected = 0;
        unsigned unrolled = 0;
    } stats;

    bool changed = false;
    bool madeProgress = true;
    while (madeProgress) {
        madeProgress = false;

        // Re-collect loops after each successful unroll, because LoopInfo
        // changes can invalidate previously collected Loop* handles.
        SmallVector<Loop *, 8> loops;
        for (Loop *L : LI) {
            collectInnermostLoops(L, loops);
        }

        // Only count on the first pass (re-collections after unrolls
        // would double-count surviving loops).
        if (!changed) {
            stats.loopsSeen = loops.size();
        }

        for (Loop *L : loops) {
            StringRef headerName = L->getHeader()->getName();

            if (!L->isLoopSimplifyForm()) {
                stats.skippedNotSimplify++;
                LLVM_DEBUG(dbgs() << "  Loop unroll: skip " << headerName
                                  << " (not in LoopSimplify form)\n");
                continue;
            }

            // UnrollLoop with PreserveLCSSA requires the whole nest to already
            // satisfy LCSSA; enforce it defensively for robustness.
            if (!L->isRecursivelyLCSSAForm(DT, LI)) {
                formLCSSARecursively(*L, DT, &LI, &SE);
            }

            // Only unroll loops with known exact trip count.
            // __loop_tripcount markers are NOT used here because they annotate
            // the *maximum* trip count, not the actual count — unrolling based
            // on a maximum would produce incorrect code when the real count is lower.
            unsigned tripCount = SE.getSmallConstantTripCount(L);
            if (tripCount == 0) {
                stats.skippedUnknownTrip++;
                LLVM_DEBUG(dbgs() << "  Loop unroll: skip " << headerName
                                  << " (unknown trip count)\n");
                continue;
            }

            // Compute body energy: sum of all blocks in the loop.
            double bodyEnergy = 0.0;
            for (BasicBlock *BB : L->getBlocks()) {
                bodyEnergy += estimator.estimate(*BB).cost;
            }

            if (bodyEnergy >= E_safe) {
                stats.skippedBodyTooLarge++;
                LLVM_DEBUG(dbgs() << "  Loop unroll: skip " << headerName
                                  << " (body energy " << bodyEnergy
                                  << " >= E_safe " << E_safe << ")\n");
                continue;
            }

            // Calculate unroll factor: how many iterations fit in E_safe.
            unsigned maxUnroll = static_cast<unsigned>(E_safe / bodyEnergy);
            unsigned unrollFactor = std::min(maxUnroll, tripCount);
            if (unrollFactor <= 1) {
                stats.skippedFactorTooSmall++;
                LLVM_DEBUG(dbgs() << "  Loop unroll: skip " << headerName
                                  << " (unroll factor " << unrollFactor
                                  << " from maxUnroll=" << maxUnroll
                                  << ", tripCount=" << tripCount << ")\n");
                continue;
            }

            // Set up unroll options.
            UnrollLoopOptions ULO;
            ULO.Count = unrollFactor;
            ULO.Force = true;
            ULO.Runtime = false;
            ULO.AllowExpensiveTripCount = false;
            ULO.UnrollRemainder = (unrollFactor == tripCount);
            ULO.ForgetAllSCEV = true;

            errs() << "  Unrolling loop at " << headerName
                   << " (trip count: " << tripCount
                   << ", body energy: " << bodyEnergy
                   << ", factor: " << unrollFactor
                   << (ULO.UnrollRemainder ? ", full" : ", partial")
                   << ")\n";

            LoopUnrollResult res = UnrollLoop(L, ULO, &LI, &SE, &DT, &AC,
                                              /*TTI=*/nullptr,
                                              /*ORE=*/nullptr,
                                              /*PreserveLCSSA=*/true);
            if (res == LoopUnrollResult::Unmodified) {
                stats.skippedLLVMRejected++;
                errs() << "  Loop unroll: LLVM rejected unroll of "
                       << headerName << "\n";
                continue;
            }

            stats.unrolled++;
            changed = true;
            madeProgress = true;
            break;
        }
    }

    // Print summary (always visible, like LoopStripMiningPass)
    unsigned skippedTotal = stats.skippedNotSimplify + stats.skippedUnknownTrip +
                            stats.skippedBodyTooLarge + stats.skippedFactorTooSmall +
                            stats.skippedLLVMRejected;
    errs() << "=== Loop Unroll: " << F.getName() << " ===\n";
    errs() << "  Innermost loops seen:            " << stats.loopsSeen << "\n";
    errs() << "  Loops unrolled:                  " << stats.unrolled << "\n";
    errs() << "  Skipped loops:                   " << skippedTotal << "\n";
    if (skippedTotal > 0) {
        errs() << "  Skip reason breakdown:\n";
        if (stats.skippedNotSimplify)
            errs() << "    - not LoopSimplify form:       "
                   << stats.skippedNotSimplify << "\n";
        if (stats.skippedUnknownTrip)
            errs() << "    - unknown trip count:           "
                   << stats.skippedUnknownTrip << "\n";
        if (stats.skippedBodyTooLarge)
            errs() << "    - body energy >= E_safe:        "
                   << stats.skippedBodyTooLarge << "\n";
        if (stats.skippedFactorTooSmall)
            errs() << "    - unroll factor <= 1:           "
                   << stats.skippedFactorTooSmall << "\n";
        if (stats.skippedLLVMRejected)
            errs() << "    - LLVM UnrollLoop rejected:     "
                   << stats.skippedLLVMRejected << "\n";
    }

    return changed;
}

PreservedAnalyses RockClimbPass::run(Function &F,
                                     FunctionAnalysisManager &AM) {
    const auto totalStart = std::chrono::steady_clock::now();

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

    bool useDistributedCkpt = resolveFeatureToggle(
        DistributedCkptOpt, ctx.params.distributedCheckpointing);
    bool addDebugMarkers =
        AddDebugMarkersOpt.getValue() || ctx.params.addDebugMarkers;

    printPassConfig(F, ctx, useDistributedCkpt, addDebugMarkers);

    // Loop unrolling optimization (before partitioning)
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

    // Algorithm 1 region formation.
    RockClimbOptimizer optimizer(*ctx.cfg, ctx.E_safe, LI, F,
                                 ctx.estimator.get());

    if (ctx.params.checkpoint_store_energy > 0 && useDistributedCkpt) {
        // Compute CkptCycles once from an initial partition result, then run
        // Algorithm 1 with Cycle = Cycle_ori + CkptCycles.
        auto prelimResult = optimizer.optimize();
        if (!prelimResult.feasible) {
            errs() << "Region partitioning failed: "
                   << prelimResult.errorMessage << "\n";
            return PreservedAnalyses::all();
        }

        DistributedCheckpointing prelimCkpt(prelimResult.regions);
        auto prelimPoints = prelimCkpt.analyze();
        auto checkpointStoreCycles = computeCheckpointStoreCycles(
            prelimPoints, ctx.params.checkpoint_store_energy);
        optimizer.setExtraBlockCosts(checkpointStoreCycles);
    }

    RockClimbOptimizer::Result result = optimizer.optimize();
    if (!result.feasible) {
        errs() << "Region partitioning failed: "
               << result.errorMessage << "\n";
        return PreservedAnalyses::all();
    }
    std::vector<CheckpointPoint> checkpointPoints;

    LLVM_DEBUG({
        dbgs() << "\nRegion boundaries (" << result.regionBoundaries.size() << "):\n";
        for (const auto &handle : result.regionBoundaries) {
            BasicBlock *BB = RockClimbOptimizer::resolveBlock(handle);
            dbgs() << "  " << getBlockName(*BB, F) << "\n";
        }

        dbgs() << "\nRegions (" << result.regions.size() << "):\n";
        for (const auto &region : result.regions) {
            BasicBlock *startBB = RockClimbOptimizer::resolveBlock(region.startBlock);
            dbgs() << "  Region starting at " << getBlockName(*startBB, F)
                   << " (blocks: " << region.blocks.size() << ")\n";
        }
    });

    // Distributed checkpointing analysis (register checkpoints)
    DistributedCheckpointing distCkpt(result.regions);

    if (useDistributedCkpt) {
        checkpointPoints = distCkpt.analyze();

        LLVM_DEBUG({
            dbgs() << "\nDistributed register checkpoints (" << checkpointPoints.size() << "):\n";
            for (const auto &ckpt : checkpointPoints) {
                dbgs() << "  Reg " << ckpt.regId << " in region "
                       << getBlockName(*ckpt.regionStart, F);
                if (ckpt.afterInst) {
                    dbgs() << " after instruction: ";
                    ckpt.afterInst->print(dbgs());
                }
                dbgs() << "\n";
            }
        });
    }

    // Convert boundaries to pointer set for instrumenter
    SmallPtrSet<BasicBlock*, 16> boundarySet;
    for (const auto &handle : result.regionBoundaries) {
        boundarySet.insert(RockClimbOptimizer::resolveBlock(handle));
    }

    // Skip entry block from voltage checks (first boundary is always entry)
    // Entry doesn't need a voltage check since execution just started
    boundarySet.erase(&F.getEntryBlock());

    // Instrument the function
    RockClimbInstrumenter instrumenter(
        *F.getParent(), CheckFnOpt, SaveRegFnOpt, addDebugMarkers);
    instrumenter.instrumentFunction(
        F, boundarySet, checkpointPoints, useDistributedCkpt);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    errs() << "\n=== RockClimb Metrics ===\n";
    errs() << "  Basic blocks: " << ctx.cfg->getBlocks().size() << "\n";
    errs() << "  Edges: " << ctx.cfg->getEdges().size() << "\n";
    errs() << "  Regions: " << result.regions.size() << "\n";
    errs() << "  Boundary checks: " << boundarySet.size() << "\n";
    errs() << "  Register checkpoints: " << checkpointPoints.size() << "\n";
    errs() << "  Compilation time (ms): "
           << llvm::format("%.3f", totalExecutionTimeMs) << "\n";
    errs() << "  Peak RSS (KB): " << getPeakRSSKb() << "\n";

    // We modified the IR
    return PreservedAnalyses::none();
}

} // namespace checkpoint

// Note: Plugin registration is in src/common/PassRegistry.cpp
// This file just provides the pass implementation
