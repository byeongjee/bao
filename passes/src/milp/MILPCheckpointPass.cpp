#include "milp/MILPCheckpointPass.h"

#include "milp/AbstractCFG.h"
#include "milp/BBFreqLoader.h"
#include "milp/CheckpointContext.h"
#include "milp/CheckpointInstrumenter.h"
#include "milp/CheckpointOptimizer.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "common/BlockUtils.h"
#include "common/PassStatistics.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"

#include <chrono>
#include <string>

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;
extern cl::opt<bool> AcceptFeasibleOpt;
extern cl::opt<double> MILPTimeLimitOpt;
extern cl::opt<bool> AddDebugMarkersOpt;
extern cl::opt<std::string> BBFreqFileOpt;

namespace checkpoint {

PreservedAnalyses MILPCheckpointPass::run(Function &F, FunctionAnalysisManager &AM) {
    const auto totalStart = std::chrono::steady_clock::now();

    // Step 1: Obtain LLVM analyses
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);

    // Load BB frequency file (required)
    if (BBFreqFileOpt.getValue().empty()) {
        report_fatal_error("MILP requires -bb-freq-file=<path> (run bb-freq-collect pipeline "
                           "first to generate BB frequency data)",
                           /*gen_crash_diag=*/false);
    }
    auto freqLoader = BBFreqLoader::load(BBFreqFileOpt.getValue(), F);
    if (!freqLoader) {
        report_fatal_error(Twine("Failed to load BB frequency file '") + BBFreqFileOpt.getValue() +
                               "' for function '" + F.getName() + "'",
                           /*gen_crash_diag=*/false);
    }
    errs() << "MILP: using BB frequency file for function " << F.getName() << "\n";

    // Step 2: Create base checkpoint context (estimator + CFG)
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(), "checkpoint pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // Step 2b: Hoist non-entry static allocas to the entry block.
    // At -O2, inlining can leave constant-size allocas in non-entry blocks.
    // StateAnalysis needs them in the entry block so they dominate all uses
    // and liveness is computed correctly.
    BasicBlock &entryBB = F.getEntryBlock();
    for (BasicBlock &BB : F) {
        if (&BB == &entryBB)
            continue;
        for (auto it = BB.begin(); it != BB.end();) {
            auto *AI = dyn_cast<AllocaInst>(&*it++);
            if (AI && isa<ConstantInt>(AI->getArraySize()))
                AI->moveBefore(entryBB, entryBB.getFirstInsertionPt());
        }
    }

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
        errs() << "Error: Failed to parse MILP config: " << MILPConfigOpt.getValue() << "\n";
        return PreservedAnalyses::all();
    }
    ctx.milpParams = *milpParamsOpt;
    ctx.energyModel =
        std::make_unique<EnergyModel>(*ctx.cfg, *ctx.stateAnalysis, *freqLoader, F, ctx.milpParams);

    // Step 5: Build abstract MILP model (loop summaries) and check feasibility.
    AbstractCFGBuildResult abstractCFG =
        buildAbstractCFG(F, LI, SE, *ctx.cfg, *ctx.stateAnalysis, *ctx.energyModel);

    MILPInput milpInput{*abstractCFG.model, *abstractCFG.model, *abstractCFG.model};
    CheckpointOptimizer optimizer(milpInput);
    optimizer.setAcceptFeasible(AcceptFeasibleOpt);
    optimizer.setTimeLimit(MILPTimeLimitOpt);

    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (NodeId block : infeasible) {
            errs() << "  " << abstractCFG.model->getNodeName(block)
                   << " (cost: " << abstractCFG.model->getBlockEnergyCost(block)
                   << ", capacity: " << ctx.milpParams.capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Count ineligible objects by type.
    unsigned ineligGlobalCount = 0, ineligAllocaCount = 0, ineligSSACount = 0;
    for (llvm::Value *V : ctx.stateAnalysis->getIneligibleObjs()) {
        if (llvm::isa<llvm::GlobalVariable>(V))
            ineligGlobalCount++;
        else if (llvm::isa<llvm::AllocaInst>(V))
            ineligAllocaCount++;
        else
            ineligSSACount++;
    }

    // Step 6: Solve MILP
    auto solveStart = std::chrono::steady_clock::now();
    if (!optimizer.solve()) {
        auto solveEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();
        double solveTimeMs =
            std::chrono::duration<double, std::milli>(solveEnd - solveStart).count();
        double totalExecutionTimeMs =
            std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

        errs() << "Optimization failed\n";

        {
            CommonStats common;
            common.passName = "MILP";
            common.functionName = F.getName().str();
            common.basicBlocks = ctx.cfg->getBlocks().size();
            common.edges = ctx.cfg->getEdges().size();
            common.candidateGlobals = ctx.stateAnalysis->getVMObjs().size();
            common.regions = 0;
            common.regionBoundaries = 0;
            common.runtimeCallsInserted = 0;
            common.compilationTimeMs = totalExecutionTimeMs;
            common.peakRSSKb = getPeakRSSKb();
            printCommonStats(errs(), common);
        }

        errs() << "  --- MILP-specific ---\n";
        errs() << "  Basic blocks (abstract):         " << abstractCFG.stats.abstractNodes << "\n";
        errs() << "  Edges (abstract):                " << abstractCFG.stats.abstractEdges << "\n";
        errs() << "  Loops seen:                      " << abstractCFG.stats.loopsSeen << "\n";
        errs() << "  Loops eligible:                  " << abstractCFG.stats.loopsEligible << "\n";
        errs() << "  Loops summarized:                " << abstractCFG.stats.loopsSummarized
               << "\n";
        errs() << "  Strip-mined loops seen:          " << abstractCFG.stats.stripMinedLoopsSeen
               << "\n";
        errs() << "  Strip-mined loops summarized:    "
               << abstractCFG.stats.stripMinedLoopsSummarized << "\n";
        errs() << "  Strip-mined loops skipped:       " << abstractCFG.stats.stripMinedLoopsSkipped
               << "\n";
        errs() << "  Ineligible globals:              " << ineligGlobalCount << "\n";
        errs() << "  Ineligible allocas:              " << ineligAllocaCount << "\n";
        errs() << "  Ineligible SSA registers:        " << ineligSSACount << "\n";
        errs() << "  MILP variables:                  " << optimizer.getNumVars() << "\n";
        errs() << "  MILP constraints:                " << optimizer.getNumConstrs() << "\n";
        errs() << "  Optimal solution:                no (solver failed)\n";
        errs() << "  Solve time (ms):                 " << llvm::format("%.3f", solveTimeMs)
               << "\n";
        if (!abstractCFG.stats.skippedReasons.empty()) {
            errs() << "  Abstract CFG skip reasons:\n";
            for (const auto &[reason, count] : abstractCFG.stats.skippedReasons) {
                errs() << "    - " << reason << ": " << count << "\n";
            }
        }

        return PreservedAnalyses::all();
    }
    auto solveEnd = std::chrono::steady_clock::now();
    double solveTimeMs = std::chrono::duration<double, std::milli>(solveEnd - solveStart).count();

    const auto &solution = optimizer.getSolution();

    if (solution.regionStarts.empty()) {
        errs() << "No region boundaries needed for function " << F.getName() << "\n";
        return PreservedAnalyses::all();
    }

    // Step 7: Instrument IR (Pass F)
    bool addDebugMarkers = AddDebugMarkersOpt.getValue() || ctx.milpParams.addDebugMarkers;
    CheckpointInstrumenter instrumenter(*F.getParent(), addDebugMarkers);
    unsigned inserted = instrumenter.instrumentFunction(F, solution, *abstractCFG.model,
                                                        *abstractCFG.model, *ctx.stateAnalysis);

    unsigned commitCount = MILPSolution::countEnabled(solution.commit);
    unsigned restoreCount = MILPSolution::countEnabled(solution.needRestore);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Statistics summary
    {
        CommonStats common;
        common.passName = "MILP";
        common.functionName = F.getName().str();
        common.basicBlocks = ctx.cfg->getBlocks().size();
        common.edges = ctx.cfg->getEdges().size();
        common.candidateGlobals = ctx.stateAnalysis->getVMObjs().size();
        common.regions = solution.regionStarts.size();
        common.regionBoundaries =
            solution.regionStarts.empty() ? 0 : solution.regionStarts.size() - 1;
        common.runtimeCallsInserted = inserted;
        common.compilationTimeMs = totalExecutionTimeMs;
        common.peakRSSKb = getPeakRSSKb();
        printCommonStats(errs(), common);
    }

    errs() << "  --- MILP-specific ---\n";
    errs() << "  Basic blocks (abstract):         " << abstractCFG.stats.abstractNodes << "\n";
    errs() << "  Edges (abstract):                " << abstractCFG.stats.abstractEdges << "\n";
    errs() << "  Loops seen:                      " << abstractCFG.stats.loopsSeen << "\n";
    errs() << "  Loops eligible:                  " << abstractCFG.stats.loopsEligible << "\n";
    errs() << "  Loops summarized:                " << abstractCFG.stats.loopsSummarized << "\n";
    errs() << "  Strip-mined loops seen:          " << abstractCFG.stats.stripMinedLoopsSeen
           << "\n";
    errs() << "  Strip-mined loops summarized:    " << abstractCFG.stats.stripMinedLoopsSummarized
           << "\n";
    errs() << "  Strip-mined loops skipped:       " << abstractCFG.stats.stripMinedLoopsSkipped
           << "\n";
    errs() << "  Ineligible globals:              " << ineligGlobalCount << "\n";
    errs() << "  Ineligible allocas:              " << ineligAllocaCount << "\n";
    errs() << "  Ineligible SSA registers:        " << ineligSSACount << "\n";
    errs() << "  MILP variables:                  " << optimizer.getNumVars() << "\n";
    errs() << "  MILP constraints:                " << optimizer.getNumConstrs() << "\n";
    if (solution.solverStatus == SolverStatus::Optimal) {
        errs() << "  Optimal solution:                yes\n";
    } else {
        errs() << "  Optimal solution:                no (MIP gap: " << solution.mipGap << ")\n";
    }
    errs() << "  Boundary commits enabled:        " << commitCount << "\n";
    errs() << "  Boundary restores enabled:       " << restoreCount << "\n";
    errs() << "  Solve time (ms):                 " << llvm::format("%.3f", solveTimeMs) << "\n";
    if (!abstractCFG.stats.skippedReasons.empty()) {
        errs() << "  Abstract CFG skip reasons:\n";
        for (const auto &[reason, count] : abstractCFG.stats.skippedReasons) {
            errs() << "    - " << reason << ": " << count << "\n";
        }
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
