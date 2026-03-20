#include "milp/MILPCheckpointPass.h"

#include "milp/AbstractCFG.h"
#include "milp/BBFreqLoader.h"
#include "milp/CheckpointContext.h"
#include "milp/CheckpointInstrumenter.h"
#include "milp/CheckpointOptimizer.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/PassStatistics.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

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

    // Skip benchmark infrastructure functions — these are timing/debug helpers
    // that should not be checkpointed (checkpointing a busy-wait delay loop
    // causes massive overhead and incorrect behavior).
    StringRef name = F.getName();
    if (name.starts_with("timing_gpio") || name.starts_with("_timing_delay") ||
        name.starts_with("debug_") || name.starts_with("uart_")) {
        PLOGI << "MILP: skipping benchmark infrastructure function " << name;
        return PreservedAnalyses::all();
    }

    // Verify EdgeSplitPass ran: every predecessor of a merge point must
    // have exactly one predecessor (a fresh split block).
    for (BasicBlock &BB : F) {
        if (BB.isEHPad())
            continue;
        SmallVector<BasicBlock *, 4> preds(predecessors(&BB));
        if (preds.size() <= 1)
            continue;
        for (BasicBlock *pred : preds) {
            unsigned predPredCount = 0;
            for ([[maybe_unused]] BasicBlock *pp : predecessors(pred))
                ++predPredCount;
            assert(predPredCount == 1 && "MILPCheckpointPass requires EdgeSplitPass — merge-point "
                                         "predecessor has != 1 predecessor");
        }
    }

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
    PLOGI << "MILP: using BB frequency file for function " << F.getName();

    // Step 2: Create base checkpoint context (estimator + CFG)
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(), "checkpoint pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            PLOGE << ctxResult.errorMessage;
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
        PLOGW << "Skipping MILP instrumentation for function " << F.getName()
              << " due to unresolved memory/call effects.";
        return PreservedAnalyses::all();
    }

    // Step 4: Parse MILP energy params and build EnergyModel (Pass C/D)
    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt) {
        PLOGE << "Error: Failed to parse MILP config: " << MILPConfigOpt.getValue();
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
        PLOGE << "Error: The following blocks exceed energy capacity:";
        for (NodeId block : infeasible) {
            PLOGE << "  " << abstractCFG.model->getNodeName(block)
                  << " (cost: " << abstractCFG.model->getBlockEnergyCost(block)
                  << ", capacity: " << ctx.milpParams.capacity << ")";
        }
        if (!StatsJsonOpt.empty()) {
            const auto totalEnd = std::chrono::steady_clock::now();
            double totalMs =
                std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
            CommonStats c;
            c.passName = "MILP";
            c.functionName = F.getName().str();
            c.basicBlocks = ctx.cfg->getBlocks().size();
            c.edges = ctx.cfg->getEdges().size();
            c.candidateGlobals = ctx.stateAnalysis->getVMObjs().size();
            c.compilationTimeMs = totalMs;
            c.peakRSSKb = getPeakRSSKb();
            json::Object root = commonStatsToJSON(c);
            root["feasible"] = false;
            root["infeasibility_reason"] = "blocks exceed energy capacity";
            root["milp_variables"] = static_cast<int64_t>(optimizer.getNumVars());
            root["milp_constraints"] = static_cast<int64_t>(optimizer.getNumConstrs());
            writeStatsJSON(StatsJsonOpt, std::move(root));
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

        PLOGE << "Optimization failed";

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
            printCommonStats(common);
        }

        PLOGI << "  --- MILP-specific ---";
        PLOGI << "  Basic blocks (abstract):         " << abstractCFG.stats.abstractNodes;
        PLOGI << "  Edges (abstract):                " << abstractCFG.stats.abstractEdges;
        PLOGI << "  Loops seen:                      " << abstractCFG.stats.loopsSeen;
        PLOGI << "  Loops eligible:                  " << abstractCFG.stats.loopsEligible;
        PLOGI << "  Loops summarized:                " << abstractCFG.stats.loopsSummarized;
        PLOGI << "  Strip-mined loops seen:          " << abstractCFG.stats.stripMinedLoopsSeen;
        PLOGI << "  Strip-mined loops summarized:    "
              << abstractCFG.stats.stripMinedLoopsSummarized;
        PLOGI << "  Strip-mined loops skipped:       " << abstractCFG.stats.stripMinedLoopsSkipped;
        PLOGI << "  Ineligible globals:              " << ineligGlobalCount;
        PLOGI << "  Ineligible allocas:              " << ineligAllocaCount;
        PLOGI << "  Ineligible SSA registers:        " << ineligSSACount;
        PLOGI << "  MILP variables:                  " << optimizer.getNumVars();
        PLOGI << "  MILP constraints:                " << optimizer.getNumConstrs();
        PLOGI << "  Optimal solution:                no (solver failed)";
        PLOGI << "  Solve time (ms):                 " << checkpoint::fmtDouble(solveTimeMs);
        if (!abstractCFG.stats.skippedReasons.empty()) {
            PLOGI << "  Abstract CFG skip reasons:";
            for (const auto &[reason, count] : abstractCFG.stats.skippedReasons) {
                PLOGI << "    - " << reason << ": " << count;
            }
        }

        if (!StatsJsonOpt.empty()) {
            CommonStats c;
            c.passName = "MILP";
            c.functionName = F.getName().str();
            c.basicBlocks = ctx.cfg->getBlocks().size();
            c.edges = ctx.cfg->getEdges().size();
            c.candidateGlobals = ctx.stateAnalysis->getVMObjs().size();
            c.compilationTimeMs = totalExecutionTimeMs;
            c.peakRSSKb = getPeakRSSKb();
            json::Object root = commonStatsToJSON(c);
            root["feasible"] = false;
            root["infeasibility_reason"] = "solver found no feasible solution";
            root["milp_variables"] = static_cast<int64_t>(optimizer.getNumVars());
            root["milp_constraints"] = static_cast<int64_t>(optimizer.getNumConstrs());
            root["optimal_solution"] = "no (solver failed)";
            root["solve_time_ms"] = solveTimeMs;
            root["boundary_commits_enabled"] = int64_t{0};
            json::Object acfg;
            acfg["abstract_nodes"] = static_cast<int64_t>(abstractCFG.stats.abstractNodes);
            acfg["abstract_edges"] = static_cast<int64_t>(abstractCFG.stats.abstractEdges);
            acfg["loops_seen"] = static_cast<int64_t>(abstractCFG.stats.loopsSeen);
            acfg["loops_eligible"] = static_cast<int64_t>(abstractCFG.stats.loopsEligible);
            acfg["loops_summarized"] = static_cast<int64_t>(abstractCFG.stats.loopsSummarized);
            acfg["strip_mined_loops_seen"] =
                static_cast<int64_t>(abstractCFG.stats.stripMinedLoopsSeen);
            acfg["strip_mined_loops_summarized"] =
                static_cast<int64_t>(abstractCFG.stats.stripMinedLoopsSummarized);
            acfg["strip_mined_loops_skipped"] =
                static_cast<int64_t>(abstractCFG.stats.stripMinedLoopsSkipped);
            root["abstract_cfg"] = std::move(acfg);
            root["ineligible_globals"] = static_cast<int64_t>(ineligGlobalCount);
            root["ineligible_allocas"] = static_cast<int64_t>(ineligAllocaCount);
            root["ineligible_ssa"] = static_cast<int64_t>(ineligSSACount);
            writeStatsJSON(StatsJsonOpt, std::move(root));
        }

        return PreservedAnalyses::all();
    }
    auto solveEnd = std::chrono::steady_clock::now();
    double solveTimeMs = std::chrono::duration<double, std::milli>(solveEnd - solveStart).count();

    const auto &solution = optimizer.getSolution();

    if (solution.r.empty()) {
        PLOGI << "No region boundaries needed for function " << F.getName();
        if (!StatsJsonOpt.empty()) {
            const auto totalEnd = std::chrono::steady_clock::now();
            double totalMs =
                std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
            CommonStats c;
            c.passName = "MILP";
            c.functionName = F.getName().str();
            c.basicBlocks = ctx.cfg->getBlocks().size();
            c.edges = ctx.cfg->getEdges().size();
            c.candidateGlobals = ctx.stateAnalysis->getVMObjs().size();
            c.compilationTimeMs = totalMs;
            c.peakRSSKb = getPeakRSSKb();
            json::Object root = commonStatsToJSON(c);
            root["feasible"] = true;
            root["milp_variables"] = static_cast<int64_t>(optimizer.getNumVars());
            root["milp_constraints"] = static_cast<int64_t>(optimizer.getNumConstrs());
            root["solve_time_ms"] = solveTimeMs;
            writeStatsJSON(StatsJsonOpt, std::move(root));
        }
        return PreservedAnalyses::all();
    }

    // Step 7: Instrument IR (Pass F)
    bool addDebugMarkers = AddDebugMarkersOpt.getValue() || ctx.milpParams.addDebugMarkers;
    CheckpointInstrumenter instrumenter(*F.getParent(), addDebugMarkers);
    unsigned inserted = instrumenter.instrumentFunction(F, solution, *abstractCFG.model,
                                                        *abstractCFG.model, *ctx.stateAnalysis);

    unsigned commitCount = MILPSolution::countEnabled(solution.s);
    unsigned restoreCount = MILPSolution::countEnabled(solution.rHat);

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
        common.regions = solution.r.size();
        common.regionBoundaries = solution.r.size();
        common.runtimeCallsInserted = inserted;
        common.compilationTimeMs = totalExecutionTimeMs;
        common.peakRSSKb = getPeakRSSKb();
        printCommonStats(common);
    }

    PLOGI << "  --- MILP-specific ---";
    PLOGI << "  Basic blocks (abstract):         " << abstractCFG.stats.abstractNodes;
    PLOGI << "  Edges (abstract):                " << abstractCFG.stats.abstractEdges;
    PLOGI << "  Loops seen:                      " << abstractCFG.stats.loopsSeen;
    PLOGI << "  Loops eligible:                  " << abstractCFG.stats.loopsEligible;
    PLOGI << "  Loops summarized:                " << abstractCFG.stats.loopsSummarized;
    PLOGI << "  Strip-mined loops seen:          " << abstractCFG.stats.stripMinedLoopsSeen;
    PLOGI << "  Strip-mined loops summarized:    " << abstractCFG.stats.stripMinedLoopsSummarized;
    PLOGI << "  Strip-mined loops skipped:       " << abstractCFG.stats.stripMinedLoopsSkipped;
    PLOGI << "  Ineligible globals:              " << ineligGlobalCount;
    PLOGI << "  Ineligible allocas:              " << ineligAllocaCount;
    PLOGI << "  Ineligible SSA registers:        " << ineligSSACount;
    PLOGI << "  MILP variables:                  " << optimizer.getNumVars();
    PLOGI << "  MILP constraints:                " << optimizer.getNumConstrs();
    if (solution.solverStatus == SolverStatus::Optimal) {
        PLOGI << "  Optimal solution:                yes";
    } else {
        PLOGI << "  Optimal solution:                no (MIP gap: " << solution.mipGap << ")";
    }
    PLOGI << "  Boundary commits enabled:        " << commitCount;
    PLOGI << "  Boundary restores enabled:       " << restoreCount;
    PLOGI << "  Solve time (ms):                 " << checkpoint::fmtDouble(solveTimeMs);
    if (!abstractCFG.stats.skippedReasons.empty()) {
        PLOGI << "  Abstract CFG skip reasons:";
        for (const auto &[reason, count] : abstractCFG.stats.skippedReasons) {
            PLOGI << "    - " << reason << ": " << count;
        }
    }

    if (!StatsJsonOpt.empty()) {
        CommonStats c;
        c.passName = "MILP";
        c.functionName = F.getName().str();
        c.basicBlocks = ctx.cfg->getBlocks().size();
        c.edges = ctx.cfg->getEdges().size();
        c.candidateGlobals = ctx.stateAnalysis->getVMObjs().size();
        c.regions = solution.r.size();
        c.regionBoundaries = solution.r.size();
        c.runtimeCallsInserted = inserted;
        c.compilationTimeMs = totalExecutionTimeMs;
        c.peakRSSKb = getPeakRSSKb();
        json::Object root = commonStatsToJSON(c);
        root["feasible"] = true;
        root["milp_variables"] = static_cast<int64_t>(optimizer.getNumVars());
        root["milp_constraints"] = static_cast<int64_t>(optimizer.getNumConstrs());
        if (solution.solverStatus == SolverStatus::Optimal) {
            root["optimal_solution"] = "yes";
        } else {
            root["optimal_solution"] =
                std::string("no (MIP gap: ") + std::to_string(solution.mipGap) + ")";
        }
        root["boundary_commits_enabled"] = static_cast<int64_t>(commitCount);
        root["boundary_restores_enabled"] = static_cast<int64_t>(restoreCount);
        root["solve_time_ms"] = solveTimeMs;
        json::Object acfg;
        acfg["abstract_nodes"] = static_cast<int64_t>(abstractCFG.stats.abstractNodes);
        acfg["abstract_edges"] = static_cast<int64_t>(abstractCFG.stats.abstractEdges);
        acfg["loops_seen"] = static_cast<int64_t>(abstractCFG.stats.loopsSeen);
        acfg["loops_eligible"] = static_cast<int64_t>(abstractCFG.stats.loopsEligible);
        acfg["loops_summarized"] = static_cast<int64_t>(abstractCFG.stats.loopsSummarized);
        acfg["strip_mined_loops_seen"] =
            static_cast<int64_t>(abstractCFG.stats.stripMinedLoopsSeen);
        acfg["strip_mined_loops_summarized"] =
            static_cast<int64_t>(abstractCFG.stats.stripMinedLoopsSummarized);
        acfg["strip_mined_loops_skipped"] =
            static_cast<int64_t>(abstractCFG.stats.stripMinedLoopsSkipped);
        root["abstract_cfg"] = std::move(acfg);
        root["ineligible_globals"] = static_cast<int64_t>(ineligGlobalCount);
        root["ineligible_allocas"] = static_cast<int64_t>(ineligAllocaCount);
        root["ineligible_ssa"] = static_cast<int64_t>(ineligSSACount);
        writeStatsJSON(StatsJsonOpt, std::move(root));
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
