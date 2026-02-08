#include "rockclimb/RockClimbPass.h"
#include "rockclimb/DistributedCheckpointing.h"
#include "rockclimb/RockClimbContext.h"
#include "rockclimb/RockClimbInstrumenter.h"
#include "rockclimb/RockClimbOptimizer.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"

#include <fstream>
#include <sstream>

using namespace llvm;

// Command line options for RockClimb pass
namespace {

static cl::opt<std::string> RockClimbConfigOpt(
    "rockclimb-config",
    cl::desc("Path to JSON configuration file for RockClimb pass"),
    cl::value_desc("filename"),
    cl::init(""));

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

} // anonymous namespace

namespace checkpoint {

// Parse RockClimb-specific parameters from JSON config
bool parseRockClimbParams(StringRef configPath, RockClimbParams &params) {
    std::ifstream file(configPath.str());
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    Expected<json::Value> parsed = json::parse(content);
    if (!parsed) {
        consumeError(parsed.takeError());
        return false;
    }

    json::Object *root = parsed->getAsObject();
    if (!root) {
        return false;
    }

    // Look for rockclimb_parameters section
    json::Object *rcParams = root->getObject("rockclimb_parameters");
    if (!rcParams) {
        return false;  // No rockclimb parameters - use defaults
    }

    // Parse individual parameters
    if (auto v = rcParams->getNumber("V_max")) {
        params.V_max = *v;
    }
    if (auto v = rcParams->getNumber("V_min")) {
        params.V_min = *v;
    }
    if (auto v = rcParams->getNumber("C_buf_uF")) {
        params.C_buf_uF = *v;
    }
    if (auto v = rcParams->getInteger("N_reg")) {
        params.N_reg = static_cast<unsigned>(*v);
    }
    if (auto v = rcParams->getNumber("E_restore_per_reg")) {
        params.E_restore_per_reg = *v;
    }
    if (auto v = rcParams->getBoolean("distributed_checkpointing")) {
        params.distributedCheckpointing = *v;
    }
    if (auto v = rcParams->getBoolean("memory_checkpointing")) {
        params.memoryCheckpointing = *v;
    }

    return true;
}

PreservedAnalyses RockClimbPass::run(Function &F,
                                      FunctionAnalysisManager &AM) {
    // Determine config path - try rockclimb-config first, fall back to energy-config
    std::string configPath = RockClimbConfigOpt;
    if (configPath.empty()) {
        // Try to use the common energy-config option
        // This requires declaring it as extern
        configPath = "";  // Will trigger error in createRockClimbContext
    }

    // Create RockClimb context
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto ctxResult = createRockClimbContext(F, LI, configPath);

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
    errs() << "  E_safe: " << ctx.E_safe << "\n";
    errs() << "  Capacity (from config): " << ctx.capacity << "\n";
    errs() << "  V_max: " << ctx.params.V_max << " V\n";
    errs() << "  V_min: " << ctx.params.V_min << " V\n";
    errs() << "  C_buf: " << ctx.params.C_buf_uF << " uF\n";
    errs() << "  Distributed checkpointing: "
           << (useDistributedCkpt ? "enabled" : "disabled") << "\n";
    errs() << "  Memory checkpointing: "
           << (useMemoryCkpt ? "enabled" : "disabled") << "\n";

    // Check feasibility - blocks that exceed E_safe individually
    RockClimbOptimizer optimizer(*ctx.cfg, ctx.E_safe, LI);
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

    // Run region partitioning
    auto result = optimizer.optimize();
    if (!result.feasible) {
        errs() << "Region partitioning failed: " << result.errorMessage << "\n";
        return PreservedAnalyses::all();
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
    std::vector<CheckpointPoint> checkpointPoints;
    MemoryCheckpointResult memCheckpoints;

    DistributedCheckpointing distCkpt(F, result.regions, result.regionBoundaries);

    if (useDistributedCkpt) {
        checkpointPoints = distCkpt.analyze();

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

// Note: Plugin registration is done in CheckpointPass.cpp
// This file just provides the pass implementation
