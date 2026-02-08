#include "milp/CheckpointAnalysisPass.h"
#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"
#include "milp/CheckpointContext.h"
#include "milp/CheckpointOptimizer.h"
#include "estimator/EnergyEstimatorFactory.h"
#include "common/LoopTripCount.h"
#include "milp/MaxCheckpointCounter.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;

namespace {

// Command line options for analysis pass
static cl::opt<unsigned> DefaultBoundOpt(
    "analysis-default-bound",
    cl::desc("Default loop trip count for unannotated loops"),
    cl::init(2));

} // anonymous namespace

namespace checkpoint {

PreservedAnalyses CheckpointAnalysisPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
    // Create checkpoint context (validates config, creates estimator and CFG)
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(),
                                             "checkpoint-analysis pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // Check feasibility
    CheckpointOptimizer optimizer(*ctx.cfg, ctx.capacity);
    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (const auto &block : infeasible) {
            errs() << "  " << block << " (cost: "
                   << ctx.cfg->getBlockInfo(block).energyCost
                   << ", capacity: " << ctx.capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Solve MILP
    if (!optimizer.solve()) {
        errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
        errs() << "Error: Optimization failed\n";
        return PreservedAnalyses::all();
    }

    // Get checkpoint block names
    auto checkpointNames = optimizer.getCheckpoints();

    // Convert names to BasicBlock pointers
    std::set<BasicBlock*> checkpoints;
    for (BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);
        if (checkpointNames.count(blockName)) {
            checkpoints.insert(&BB);
        }
    }

    // Step 3: Extract loop bounds from __loop_tripcount markers
    auto loopBounds = LoopTripCount::extractBounds(F, *ctx.loopInfo);

    // Step 4: Count max checkpoints using DP
    MaxCheckpointCounter counter(F, *ctx.loopInfo, checkpoints);
    counter.setLoopBounds(loopBounds);
    counter.setDefaultBound(DefaultBoundOpt);
    CountResult result = counter.compute();

    // Step 5: Output results
    errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
    errs() << "Configuration:\n";
    errs() << "  Energy capacity: " << ctx.capacity << "\n";
    errs() << "  Default loop bound: " << DefaultBoundOpt << "\n";
    errs() << "\n";

    errs() << "Checkpoints placed (from MILP): " << checkpointNames.size()
           << " block(s)\n";
    if (!checkpointNames.empty()) {
        errs() << "  ";
        bool first = true;
        for (const auto &name : checkpointNames) {
            if (!first) errs() << ", ";
            errs() << name;
            first = false;
        }
        errs() << "\n";
    }
    errs() << "\n";

    errs() << "Loop bounds:\n";
    if (loopBounds.empty()) {
        errs() << "  (none annotated, using default: " << DefaultBoundOpt << ")\n";
    } else {
        for (const auto &[loop, bound] : loopBounds) {
            BasicBlock *Header = loop->getHeader();
            errs() << "  Loop @ " << Header->getName() << ": " << bound
                   << " iterations (from __loop_tripcount)\n";
        }
    }
    errs() << "\n";

    errs() << "Result:\n";
    errs() << "  Maximum checkpoints on any path: " << result.maxCheckpoints
           << "\n";

    if (!result.criticalPath.empty()) {
        errs() << "  Critical path: ";
        for (size_t i = 0; i < result.criticalPath.size() && i < 10; ++i) {
            if (i > 0) errs() << " -> ";
            errs() << result.criticalPath[i]->getName();
        }
        if (result.criticalPath.size() > 10) {
            errs() << " -> ... (" << (result.criticalPath.size() - 10)
                   << " more)";
        }
        errs() << "\n";
    }

    errs() << "\n";
    errs() << "WARNING: __loop_tripcount markers may affect optimization.\n";
    errs() << "         Consider running analysis on -O0 builds.\n";
    errs() << "\n";

    // Analysis pass doesn't modify IR
    return PreservedAnalyses::all();
}

} // namespace checkpoint

// Plugin registration - extends the existing registration
// Note: Plugin registration is in src/common/PassRegistry.cpp
