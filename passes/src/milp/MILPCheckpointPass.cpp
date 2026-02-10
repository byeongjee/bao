#include "milp/MILPCheckpointPass.h"
#include "milp/CheckpointContext.h"
#include "milp/CheckpointInstrumenter.h"
#include "milp/CheckpointOptimizer.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;
extern cl::opt<bool> AcceptFeasibleOpt;

namespace checkpoint {

PreservedAnalyses MILPCheckpointPass::run(Function &F,
                                       FunctionAnalysisManager &AM) {
    // Step 1: Obtain LLVM analyses
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &BFI = AM.getResult<BlockFrequencyAnalysis>(F);

    // Step 2: Create base checkpoint context (estimator + CFG)
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(),
                                             "checkpoint pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // Step 3: Run StateAnalysis (Pass B)
    ctx.stateAnalysis =
        std::make_unique<StateAnalysis>(F, LI, AA, DT, *ctx.cfg);

    // Step 4: Parse MILP energy params and build EnergyModel (Pass C/D)
    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt) {
        errs() << "Error: Failed to parse MILP config: "
               << MILPConfigOpt.getValue() << "\n";
        return PreservedAnalyses::all();
    }
    ctx.milpParams = *milpParamsOpt;
    ctx.energyModel = std::make_unique<EnergyModel>(
        *ctx.cfg, *ctx.stateAnalysis, BFI, F, ctx.milpParams);

    // Step 5: Build MILP input and check feasibility (Pass E)
    MILPInput milpInput{*ctx.cfg, *ctx.stateAnalysis, *ctx.energyModel};
    CheckpointOptimizer optimizer(milpInput);
    optimizer.setAcceptFeasible(AcceptFeasibleOpt);

    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (const auto &block : infeasible) {
            errs() << "  " << block << " (cost: "
                   << ctx.cfg->getBlockInfo(block).energyCost
                   << ", capacity: " << ctx.milpParams.capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Step 6: Solve MILP
    if (!optimizer.solve()) {
        errs() << "Optimization failed\n";
        return PreservedAnalyses::all();
    }

    const auto &solution = optimizer.getSolution();

    if (solution.regionStarts.empty()) {
        errs() << "No region boundaries needed for function " << F.getName()
               << "\n";
        return PreservedAnalyses::all();
    }

    // Report solution
    errs() << "MILP solution for " << F.getName() << ":\n";
    errs() << "  Region starts (" << solution.regionStarts.size() << "): ";
    bool first = true;
    for (const auto &rs : solution.regionStarts) {
        if (!first) errs() << ", ";
        errs() << rs;
        first = false;
    }
    errs() << "\n";
    errs() << "  Enabled checkpoint stores: "
           << solution.enabledDefStores.size() << "\n";
    errs() << "  VM-placed globals: ";
    unsigned vmCount = 0;
    for (const auto &[gv, inVm] : solution.vmPlacement) {
        if (inVm) vmCount++;
    }
    errs() << vmCount << " / " << solution.vmPlacement.size() << "\n";
    errs() << "  Objective value: " << solution.objectiveValue << "\n";
    if (solution.solverStatus == SolverStatus::Feasible) {
        errs() << "  Solver status: FEASIBLE (MIP gap: " << solution.mipGap
               << ")\n";
    }

    // Step 7: Instrument IR (Pass F)
    CheckpointInstrumenter instrumenter(*F.getParent());
    instrumenter.instrumentFunction(F, solution, *ctx.stateAnalysis);

    return PreservedAnalyses::none();
}

} // namespace checkpoint
