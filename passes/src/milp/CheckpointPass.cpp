#include "milp/CheckpointPass.h"
#include "common/CFGAnalysis.h"
#include "milp/CheckpointContext.h"
#include "milp/CheckpointInstrumenter.h"
#include "milp/CheckpointOptimizer.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;

namespace {

static cl::opt<std::string> CheckpointFnOpt(
    "checkpoint-function",
    cl::desc("Name of checkpoint function to insert"),
    cl::init("__checkpoint"));

} // anonymous namespace

namespace checkpoint {

PreservedAnalyses CheckpointPass::run(Function &F,
                                       FunctionAnalysisManager &AM) {
    // Create checkpoint context (validates config, creates estimator and CFG)
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(),
                                             "checkpoint pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;
    std::string checkpointFnName = CheckpointFnOpt;

    // Check feasibility
    CheckpointOptimizer optimizer(*ctx.cfg, ctx.capacity);
    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (const auto &block : infeasible) {
            errs() << "  " << block << " (cost: "
                   << ctx.cfg->getBlockInfo(block).energyCost
                   << ", capacity: " << ctx.capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Step 4: Solve MILP
    if (!optimizer.solve()) {
        errs() << "Optimization failed\n";
        return PreservedAnalyses::all();
    }

    auto checkpoints = optimizer.getCheckpoints();

    if (checkpoints.empty()) {
        errs() << "No checkpoints needed for function " << F.getName() << "\n";
        return PreservedAnalyses::all();
    }

    errs() << "Inserting " << checkpoints.size() << " checkpoint(s) in "
           << F.getName() << ":\n";
    for (const auto &cp : checkpoints) {
        errs() << "  " << cp << "\n";
    }

    // Insert checkpoint calls
    CheckpointInstrumenter instrumenter(*F.getParent(), checkpointFnName);
    instrumenter.instrumentFunction(F, checkpoints);

    // We modified the IR
    return PreservedAnalyses::none();
}

} // namespace checkpoint
