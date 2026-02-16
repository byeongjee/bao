#include "milp/CheckpointAnalysisPass.h"
#include "common/BlockUtils.h"
#include "milp/CheckpointContext.h"
#include "milp/CheckpointOptimizer.h"
#include "milp/EnergyModel.h"
#include "milp/ModelViews.h"
#include "milp/StateAnalysis.h"
#include "common/LoopTripCount.h"
#include "milp/MaxCheckpointCounter.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;

namespace {

static cl::opt<unsigned> DefaultBoundOpt(
    "analysis-default-bound",
    cl::desc("Default loop trip count for unannotated loops"),
    cl::init(2));

} // anonymous namespace

namespace checkpoint {

PreservedAnalyses CheckpointAnalysisPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
    // Step 1: Obtain LLVM analyses
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &BFI = AM.getResult<BlockFrequencyAnalysis>(F);

    // Step 2: Create base checkpoint context
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(),
                                             "checkpoint-analysis pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // Step 3: Run StateAnalysis
    ctx.stateAnalysis =
        std::make_unique<StateAnalysis>(F, LI, AA, DT, *ctx.cfg);
    if (ctx.stateAnalysis->hasAnalysisErrors()) {
        errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
        ctx.stateAnalysis->printAnalysisErrors(errs());
        errs() << "Aborting MILP analysis for this function due to unresolved "
                  "memory/call effects.\n";
        return PreservedAnalyses::all();
    }

    // Step 4: Build EnergyModel
    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt) {
        errs() << "Error: Failed to parse MILP config: "
               << MILPConfigOpt.getValue() << "\n";
        return PreservedAnalyses::all();
    }
    ctx.milpParams = *milpParamsOpt;
    ctx.energyModel = std::make_unique<EnergyModel>(
        *ctx.cfg, *ctx.stateAnalysis, BFI, F, ctx.milpParams);

    // Step 5: Solve MILP
    ConcreteMILPViewAdapter modelView(*ctx.cfg, *ctx.stateAnalysis,
                                      *ctx.energyModel);
    MILPInput milpInput{modelView, modelView, modelView};
    CheckpointOptimizer optimizer(milpInput);

    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (const auto &block : infeasible) {
            errs() << "  " << block << " (cost: "
                   << modelView.getBlockEnergyCost(block)
                   << ", capacity: " << ctx.milpParams.capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    if (!optimizer.solve()) {
        errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
        errs() << "Error: Optimization failed\n";
        return PreservedAnalyses::all();
    }

    // Extract region starts from solution
    const auto &solution = optimizer.getSolution();
    auto checkpointNames = solution.regionStarts;

    // Convert names to BasicBlock pointers
    std::set<BasicBlock*> checkpoints;
    for (BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);
        if (checkpointNames.count(blockName)) {
            checkpoints.insert(&BB);
        }
    }

    // Extract loop bounds from __loop_tripcount markers
    auto loopBounds = LoopTripCount::extractBounds(F, *ctx.loopInfo);

    // Count max checkpoints using DP
    MaxCheckpointCounter counter(F, *ctx.loopInfo, checkpoints);
    counter.setLoopBounds(loopBounds);
    counter.setDefaultBound(DefaultBoundOpt);
    CountResult result = counter.compute();

    // Output results
    errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
    errs() << "Configuration:\n";
    errs() << "  Energy capacity: " << ctx.milpParams.capacity << "\n";
    errs() << "  Default loop bound: " << DefaultBoundOpt << "\n";
    errs() << "\n";

    errs() << "Region starts (from MILP): " << checkpointNames.size()
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

    unsigned commitCount = 0;
    for (const auto &[key, enabled] : solution.commit) {
        (void)key;
        if (enabled)
            commitCount++;
    }
    errs() << "Boundary commits enabled: " << commitCount << "\n";
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
