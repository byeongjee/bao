#include "milp/MILPCheckpointPass.h"

#include "milp/AbstractCFG.h"
#include "milp/CheckpointContext.h"
#include "milp/CheckpointInstrumenter.h"
#include "milp/CheckpointOptimizer.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"

#include <chrono>

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;
extern cl::opt<bool> AcceptFeasibleOpt;

namespace checkpoint {

PreservedAnalyses MILPCheckpointPass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
    const auto totalStart = std::chrono::steady_clock::now();

    // Step 1: Obtain LLVM analyses
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);
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
    ctx.stateAnalysis = std::make_unique<StateAnalysis>(F, AA, *ctx.cfg);
    if (ctx.stateAnalysis->hasAnalysisErrors()) {
        ctx.stateAnalysis->printAnalysisErrors(errs());
        errs() << "Skipping MILP instrumentation for function " << F.getName()
               << " due to unresolved memory/call effects.\n";
        return PreservedAnalyses::all();
    }

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

    // Step 5: Build abstract MILP model (loop summaries) and check feasibility.
    AbstractCFGBuildResult abstractCFG =
        buildAbstractCFG(F, LI, SE, *ctx.cfg, *ctx.stateAnalysis, *ctx.energyModel);

    MILPInput milpInput{*abstractCFG.model, *abstractCFG.model, *abstractCFG.model};
    CheckpointOptimizer optimizer(milpInput);
    optimizer.setAcceptFeasible(AcceptFeasibleOpt);

    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (NodeId block : infeasible) {
            errs() << "  " << abstractCFG.model->getNodeName(block) << " (cost: "
                   << abstractCFG.model->getBlockEnergyCost(block)
                   << ", capacity: " << ctx.milpParams.capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Step 6: Solve MILP
    auto solveStart = std::chrono::steady_clock::now();
    if (!optimizer.solve()) {
        errs() << "Optimization failed\n";
        return PreservedAnalyses::all();
    }
    auto solveEnd = std::chrono::steady_clock::now();
    double solveTimeMs =
        std::chrono::duration<double, std::milli>(solveEnd - solveStart).count();

    const auto &solution = optimizer.getSolution();

    if (solution.regionStarts.empty()) {
        errs() << "No region boundaries needed for function " << F.getName()
               << "\n";
        return PreservedAnalyses::all();
    }

    // Step 7: Instrument IR (Pass F)
    CheckpointInstrumenter instrumenter(*F.getParent());
    unsigned inserted =
        instrumenter.instrumentFunction(F, solution, *abstractCFG.model,
                                        *abstractCFG.model,
                                        *ctx.stateAnalysis);

    unsigned commitCount = 0;
    for (const auto &[key, enabled] : solution.commit) {
        (void)key;
        if (enabled)
            commitCount++;
    }
    unsigned restoreCount = 0;
    for (const auto &[key, enabled] : solution.needRestore) {
        (void)key;
        if (enabled)
            restoreCount++;
    }

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Statistics summary
    errs() << "=== MILP Checkpoint Insertion Statistics ===\n";
    errs() << "  Basic blocks (concrete):         " << ctx.cfg->getBlocks().size()
           << "\n";
    errs() << "  Edges (concrete):                " << ctx.cfg->getEdges().size()
           << "\n";
    errs() << "  Basic blocks (abstract):         " << abstractCFG.stats.abstractNodes
           << "\n";
    errs() << "  Edges (abstract):                " << abstractCFG.stats.abstractEdges
           << "\n";
    errs() << "  Loops seen:                      " << abstractCFG.stats.loopsSeen
           << "\n";
    errs() << "  Loops eligible:                  " << abstractCFG.stats.loopsEligible
           << "\n";
    errs() << "  Loops summarized:                "
           << abstractCFG.stats.loopsSummarized << "\n";
    errs() << "  Global variables:                "
           << ctx.stateAnalysis->getVMObjs().size() << "\n";
    errs() << "  MILP variables:                  " << optimizer.getNumVars() << "\n";
    errs() << "  MILP constraints:                " << optimizer.getNumConstrs()
           << "\n";
    if (solution.solverStatus == SolverStatus::Optimal) {
        errs() << "  Optimal solution:                yes\n";
    } else {
        errs() << "  Optimal solution:                no (MIP gap: "
               << solution.mipGap << ")\n";
    }
    errs() << "  Regions:                         " << solution.regionStarts.size()
           << "\n";
    errs() << "  Region boundaries inserted:      "
           << (solution.regionStarts.empty() ? 0 : solution.regionStarts.size() - 1)
           << "\n";
    errs() << "  Boundary commits enabled:        " << commitCount << "\n";
    errs() << "  Boundary restores enabled:       " << restoreCount << "\n";
    errs() << "  Runtime calls inserted:          " << inserted << "\n";
    errs() << "  Solve time (ms):                 "
           << llvm::format("%.3f", solveTimeMs) << "\n";
    errs() << "  Total execution time (ms):       "
           << llvm::format("%.3f", totalExecutionTimeMs) << "\n";
    if (!abstractCFG.stats.skippedReasons.empty()) {
        errs() << "  Abstract CFG skip reasons:\n";
        for (const auto &[reason, count] : abstractCFG.stats.skippedReasons) {
            errs() << "    - " << reason << ": " << count << "\n";
        }
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
