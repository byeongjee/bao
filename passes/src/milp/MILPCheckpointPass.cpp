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
extern cl::opt<double> MILPGapOpt;
extern cl::opt<std::string> MILPLogFileOpt;
extern cl::opt<bool> MILPCoarseAllocationOpt;
extern cl::opt<std::string> BBFreqFileOpt;

namespace {

void printAbstractCFGStats(const checkpoint::AbstractCFGStats &stats) {
    PLOGI << "  Abstract CFG blocks:            " << stats.abstractNodes;
    PLOGI << "  Abstract CFG edges:             " << stats.abstractEdges;
    PLOGI << "  Loops seen:                      " << stats.loopsSeen;
    PLOGI << "  Loops eligible:                  " << stats.loopsEligible;
    PLOGI << "  Loops summarized:                " << stats.loopsSummarized;
    PLOGI << "  Strip-mined loops seen:          " << stats.stripMinedLoopsSeen;
    PLOGI << "  Strip-mined loops summarized:    " << stats.stripMinedLoopsSummarized;
    PLOGI << "  Strip-mined loops skipped:       " << stats.stripMinedLoopsSkipped;
    if (!stats.skippedReasons.empty()) {
        PLOGI << "  Abstract CFG skip reasons:";
        for (const auto &[reason, count] : stats.skippedReasons) {
            PLOGI << "    - " << reason << ": " << count;
        }
    }
}

void appendAbstractCFGStatsToJSON(llvm::json::Object &root,
                                  const checkpoint::AbstractCFGStats &stats) {
    root["abstract_cfg_blocks"] = static_cast<int64_t>(stats.abstractNodes);
    root["abstract_cfg_edges"] = static_cast<int64_t>(stats.abstractEdges);
    root["abstract_cfg_size"] = static_cast<int64_t>(stats.abstractNodes);

    json::Object acfg;
    acfg["abstract_nodes"] = static_cast<int64_t>(stats.abstractNodes);
    acfg["abstract_edges"] = static_cast<int64_t>(stats.abstractEdges);
    acfg["loops_seen"] = static_cast<int64_t>(stats.loopsSeen);
    acfg["loops_eligible"] = static_cast<int64_t>(stats.loopsEligible);
    acfg["loops_summarized"] = static_cast<int64_t>(stats.loopsSummarized);
    acfg["strip_mined_loops_seen"] = static_cast<int64_t>(stats.stripMinedLoopsSeen);
    acfg["strip_mined_loops_summarized"] = static_cast<int64_t>(stats.stripMinedLoopsSummarized);
    acfg["strip_mined_loops_skipped"] = static_cast<int64_t>(stats.stripMinedLoopsSkipped);
    root["abstract_cfg"] = std::move(acfg);
}

void printMILPProblemSizeStats(const checkpoint::MILPProblemSizeStats &stats) {
    PLOGI << "  MILP variables (before presolve): " << stats.variablesBeforePresolve;
    PLOGI << "  MILP constraints (before presolve): " << stats.constraintsBeforePresolve;
    PLOGI << "  MILP variables (after presolve):  " << stats.variablesAfterPresolve;
    PLOGI << "  MILP constraints (after presolve): " << stats.constraintsAfterPresolve;
}

void appendMILPProblemSizeStatsToJSON(llvm::json::Object &root,
                                      const checkpoint::MILPProblemSizeStats &stats) {
    root["milp_variables"] = static_cast<int64_t>(stats.variablesBeforePresolve);
    root["milp_constraints"] = static_cast<int64_t>(stats.constraintsBeforePresolve);
    root["milp_presolved_variables"] = static_cast<int64_t>(stats.variablesAfterPresolve);
    root["milp_presolved_constraints"] = static_cast<int64_t>(stats.constraintsAfterPresolve);
}

const char *milpAllocationModeLabel(bool coarseAllocation) {
    return coarseAllocation ? "coarse" : "regional";
}

void printMILPAllocationMode(bool coarseAllocation) {
    PLOGI << "  MILP allocation mode:           " << milpAllocationModeLabel(coarseAllocation);
}

void appendMILPAllocationModeToJSON(llvm::json::Object &root, bool coarseAllocation) {
    root["milp_allocation_mode"] = milpAllocationModeLabel(coarseAllocation);
}

void appendVMPlacementToJSON(llvm::json::Object &root, unsigned vmPlacedGlobals,
                             const std::vector<std::pair<std::string, unsigned>> &vmPlacedDetail) {
    root["vm_placed_globals"] = static_cast<int64_t>(vmPlacedGlobals);
    json::Array vmVars;
    vmVars.reserve(vmPlacedDetail.size());
    for (const auto &[name, blocks] : vmPlacedDetail) {
        json::Object item;
        item["name"] = name;
        item["blocks"] = static_cast<int64_t>(blocks);
        vmVars.push_back(std::move(item));
    }
    root["vm_placed_variables"] = std::move(vmVars);
}

void printIneligibleStateStats(unsigned ineligGlobalCount, unsigned ineligAllocaCount,
                               unsigned ineligSSACount) {
    PLOGI << "  Ineligible globals:              " << ineligGlobalCount;
    PLOGI << "  Ineligible allocas:              " << ineligAllocaCount;
    PLOGI << "  Ineligible SSA registers:        " << ineligSSACount;
}

} // namespace

namespace checkpoint {

PreservedAnalyses MILPCheckpointPass::run(Function &F, FunctionAnalysisManager &AM) {
    const auto totalStart = std::chrono::steady_clock::now();
    checkpoint::initLogging();

    // Skip benchmark infrastructure functions — these are timing/debug helpers
    // that should not be checkpointed (checkpointing a busy-wait delay loop
    // causes massive overhead and incorrect behavior).
    StringRef name = F.getName();
    if (name.starts_with("timing_gpio") || name.starts_with("_timing_delay") ||
        name.starts_with("debug_") || name.starts_with("uart_")) {
        PLOGI << "MILP: skipping benchmark infrastructure function " << name;
        return PreservedAnalyses::all();
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
            report_fatal_error(Twine("MILP checkpoint pass: ") + ctxResult.errorMessage,
                               /*gen_crash_diag=*/false);
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // All exit paths report the same base fields; per-path fields
    // (regions, boundaries, runtime calls) are set at each site.
    auto makeCommonStats = [&F, &ctx](double compilationTimeMs) {
        CommonStats c;
        c.passName = "MILP";
        c.functionName = F.getName().str();
        c.basicBlocks = ctx.cfg->getBlocks().size();
        c.edges = ctx.cfg->getEdges().size();
        c.candidateGlobals = ctx.stateAnalysis->getVMObjs().size();
        c.compilationTimeMs = compilationTimeMs;
        c.peakRSSKb = getPeakRSSKb();
        return c;
    };

    // Step 2b: Hoist non-entry static allocas to the entry block.
    // At -O2, inlining can leave constant-size allocas in non-entry blocks.
    // StateAnalysis needs them in the entry block so they dominate all uses
    // and liveness is computed correctly.
    BasicBlock &entryBB = F.getEntryBlock();
    bool movedAllocas = false;
    for (BasicBlock &BB : F) {
        if (&BB == &entryBB)
            continue;
        for (auto it = BB.begin(); it != BB.end();) {
            auto *AI = dyn_cast<AllocaInst>(&*it++);
            if (AI && isa<ConstantInt>(AI->getArraySize())) {
                AI->moveBefore(entryBB, entryBB.getFirstInsertionPt());
                movedAllocas = true;
            }
        }
    }
    // Exit paths below this point must not claim all analyses are preserved
    // if the hoisting above already mutated the IR.
    auto preservedOnSkip = [&movedAllocas] {
        return movedAllocas ? PreservedAnalyses::none() : PreservedAnalyses::all();
    };

    // Step 3: Run StateAnalysis (Pass B)
    ctx.stateAnalysis = std::make_unique<StateAnalysis>(F, AA, *ctx.cfg);
    if (ctx.stateAnalysis->hasAnalysisErrors()) {
        ctx.stateAnalysis->printAnalysisErrors(errs());
        report_fatal_error(Twine("MILP checkpoint pass: unresolved memory/call effects in "
                                 "function '") +
                               F.getName() +
                               "' (see errors above); cannot guarantee checkpoint correctness",
                           /*gen_crash_diag=*/false);
    }

    // Step 4: Parse MILP energy params and build EnergyModel (Pass C/D)
    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt) {
        report_fatal_error(Twine("MILP checkpoint pass: failed to parse MILP config: ") +
                               MILPConfigOpt.getValue(),
                           /*gen_crash_diag=*/false);
    }
    ctx.milpParams = *milpParamsOpt;
    ctx.energyModel =
        std::make_unique<EnergyModel>(*ctx.cfg, *ctx.stateAnalysis, *freqLoader, F, ctx.milpParams);

    // Step 5: Build abstract MILP model (loop summaries) and check feasibility.
    AbstractCFGBuildResult abstractCFG =
        buildAbstractCFG(F, LI, SE, *ctx.cfg, *ctx.stateAnalysis, *ctx.energyModel);

    MILPInput milpInput{*abstractCFG.model, *abstractCFG.model, *abstractCFG.model};
    CheckpointOptimizer optimizer(milpInput);
    optimizer.setCoarseAllocation(MILPCoarseAllocationOpt.getValue());
    optimizer.setAcceptFeasible(AcceptFeasibleOpt);
    optimizer.setTimeLimit(MILPTimeLimitOpt);
    optimizer.setMIPGap(MILPGapOpt);
    optimizer.setLogFile(MILPLogFileOpt);
    const bool coarseAllocation = optimizer.isCoarseAllocationEnabled();

    // Count ineligible objects by type once so every exit path reports them.
    unsigned ineligGlobalCount = 0, ineligAllocaCount = 0, ineligSSACount = 0;
    for (llvm::Value *V : ctx.stateAnalysis->getIneligibleObjs()) {
        if (llvm::isa<llvm::GlobalVariable>(V))
            ineligGlobalCount++;
        else if (llvm::isa<llvm::AllocaInst>(V))
            ineligAllocaCount++;
        else
            ineligSSACount++;
    }

    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        PLOGE << "Error: The following blocks exceed energy capacity:";
        for (NodeId block : infeasible) {
            PLOGE << "  " << abstractCFG.model->getNodeName(block)
                  << " (cost: " << abstractCFG.model->getBlockEnergyCost(block)
                  << ", capacity: " << ctx.milpParams.capacity << ")";
        }

        const auto totalEnd = std::chrono::steady_clock::now();
        double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        CommonStats common = makeCommonStats(totalMs);
        printCommonStats(common);

        PLOGI << "  --- MILP-specific ---";
        printAbstractCFGStats(abstractCFG.stats);
        printIneligibleStateStats(ineligGlobalCount, ineligAllocaCount, ineligSSACount);
        printMILPAllocationMode(coarseAllocation);
        PLOGI << "  Optimal solution:                no (blocks exceed energy capacity)";

        if (!StatsJsonOpt.empty()) {
            CommonStats c = makeCommonStats(totalMs);
            json::Object root = commonStatsToJSON(c);
            root["feasible"] = false;
            root["infeasibility_reason"] = "blocks exceed energy capacity";
            appendMILPAllocationModeToJSON(root, coarseAllocation);
            root["optimal_solution"] = "no (blocks exceed energy capacity)";
            appendAbstractCFGStatsToJSON(root, abstractCFG.stats);
            root["ineligible_globals"] = static_cast<int64_t>(ineligGlobalCount);
            root["ineligible_allocas"] = static_cast<int64_t>(ineligAllocaCount);
            root["ineligible_ssa"] = static_cast<int64_t>(ineligSSACount);
            writeStatsJSON(StatsJsonOpt, std::move(root));
        }
        return preservedOnSkip();
    }

    const auto &problemSizeStats = optimizer.getProblemSizeStats();

    // Step 6: Solve MILP
    auto solveStart = std::chrono::steady_clock::now();
    if (!optimizer.solve()) {
        auto solveEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();
        double solveTimeMs =
            std::chrono::duration<double, std::milli>(solveEnd - solveStart).count();
        double totalExecutionTimeMs =
            std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

        // Report why the solve failed. A timeout is not infeasibility: only
        // ProvenInfeasible means the model has no solution.
        const char *failureLog = "Optimization failed";
        const char *optimalSolutionLine = "no (solver failed)";
        const char *failureReason = "solver failed";
        switch (optimizer.getSolveFailure()) {
        case SolveFailure::ProvenInfeasible:
            failureLog = "Optimization infeasible: solver proved the model infeasible";
            optimalSolutionLine = "no (proven infeasible)";
            failureReason = "solver proved model infeasible";
            break;
        case SolveFailure::TimeLimitNoSolution:
            failureLog = "Optimization timed out: no feasible solution found within "
                         "the time limit";
            optimalSolutionLine = "no (time limit, no solution found)";
            failureReason = "solver hit the time limit before finding any feasible solution";
            break;
        case SolveFailure::FeasibleNotAccepted:
            failureLog = "Optimization stopped with a feasible but unproven solution; "
                         "rerun with -milp-accept-feasible to use it";
            optimalSolutionLine = "no (feasible, not proven optimal)";
            failureReason = "feasible solution found but optimality unproven "
                            "(-milp-accept-feasible to use it)";
            break;
        default:
            break;
        }
        PLOGE << failureLog;

        {
            CommonStats common = makeCommonStats(totalExecutionTimeMs);
            common.regions = 0;
            common.regionBoundaries = 0;
            common.runtimeCallsInserted = 0;
            printCommonStats(common);
        }

        PLOGI << "  --- MILP-specific ---";
        printAbstractCFGStats(abstractCFG.stats);
        printIneligibleStateStats(ineligGlobalCount, ineligAllocaCount, ineligSSACount);
        printMILPAllocationMode(coarseAllocation);
        printMILPProblemSizeStats(problemSizeStats);
        PLOGI << "  Optimal solution:                " << optimalSolutionLine;
        PLOGI << "  Solve time (ms):                 " << checkpoint::fmtDouble(solveTimeMs);

        if (!StatsJsonOpt.empty()) {
            CommonStats c = makeCommonStats(totalExecutionTimeMs);
            json::Object root = commonStatsToJSON(c);
            root["feasible"] = false;
            root["infeasibility_reason"] = failureReason;
            appendMILPAllocationModeToJSON(root, coarseAllocation);
            appendMILPProblemSizeStatsToJSON(root, problemSizeStats);
            root["optimal_solution"] = optimalSolutionLine;
            root["solve_time_ms"] = solveTimeMs;
            root["boundary_commits_enabled"] = int64_t{0};
            root["boundary_restores_enabled"] = int64_t{0};
            appendAbstractCFGStatsToJSON(root, abstractCFG.stats);
            root["ineligible_globals"] = static_cast<int64_t>(ineligGlobalCount);
            root["ineligible_allocas"] = static_cast<int64_t>(ineligAllocaCount);
            root["ineligible_ssa"] = static_cast<int64_t>(ineligSSACount);
            writeStatsJSON(StatsJsonOpt, std::move(root));
        }

        return preservedOnSkip();
    }
    auto solveEnd = std::chrono::steady_clock::now();
    double solveTimeMs = std::chrono::duration<double, std::milli>(solveEnd - solveStart).count();

    const auto &solution = optimizer.getSolution();
    unsigned commitCount = MILPSolution::countEnabled(solution.s);
    unsigned restoreCount = MILPSolution::countEnabled(solution.rHat);

    // VM placement usage: among the eligible candidate globals (getVMObjs(),
    // i.e. candidate_globals / V_elig), how many the optimizer actually placed
    // in VM (m=true at >=1 block), with per-variable raw block counts.
    // Ineligible objects (forced m=true in extractSolution) are excluded by
    // iterating only over getVMObjs().
    std::map<llvm::Value *, unsigned> vmBlockCounts;
    for (const auto &[key, placed] : solution.m) {
        if (placed)
            ++vmBlockCounts[key.second];
    }
    std::vector<std::pair<std::string, unsigned>> vmPlacedDetail;
    for (llvm::GlobalVariable *GV : ctx.stateAnalysis->getVMObjs()) {
        auto it = vmBlockCounts.find(GV);
        if (it != vmBlockCounts.end() && it->second > 0)
            vmPlacedDetail.emplace_back(GV->getName().str(), it->second);
    }
    unsigned vmPlacedGlobals = vmPlacedDetail.size();
    unsigned candidateGlobalCount = ctx.stateAnalysis->getVMObjs().size();

    if (solution.r.empty()) {
        PLOGI << "No region boundaries needed for function " << F.getName();

        const auto totalEnd = std::chrono::steady_clock::now();
        double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        CommonStats common = makeCommonStats(totalMs);
        printCommonStats(common);

        PLOGI << "  --- MILP-specific ---";
        printAbstractCFGStats(abstractCFG.stats);
        printIneligibleStateStats(ineligGlobalCount, ineligAllocaCount, ineligSSACount);
        printMILPAllocationMode(coarseAllocation);
        printMILPProblemSizeStats(problemSizeStats);
        if (solution.solverStatus == SolverStatus::Optimal) {
            PLOGI << "  Optimal solution:                yes";
        } else {
            PLOGI << "  Optimal solution:                no (MIP gap: " << solution.mipGap << ")";
        }
        PLOGI << "  Boundary commits enabled:        " << commitCount;
        PLOGI << "  Boundary restores enabled:       " << restoreCount;
        PLOGI << "  VM-placed globals (of cand.):    " << vmPlacedGlobals << " / "
              << candidateGlobalCount;
        for (const auto &[name, blocks] : vmPlacedDetail)
            PLOGI << "      " << name << ": " << blocks << " block(s)";
        PLOGI << "  Solve time (ms):                 " << checkpoint::fmtDouble(solveTimeMs);

        if (!StatsJsonOpt.empty()) {
            CommonStats c = makeCommonStats(totalMs);
            json::Object root = commonStatsToJSON(c);
            root["feasible"] = true;
            appendMILPAllocationModeToJSON(root, coarseAllocation);
            appendMILPProblemSizeStatsToJSON(root, problemSizeStats);
            if (solution.solverStatus == SolverStatus::Optimal) {
                root["optimal_solution"] = "yes";
            } else {
                root["optimal_solution"] =
                    std::string("no (MIP gap: ") + std::to_string(solution.mipGap) + ")";
            }
            root["boundary_commits_enabled"] = static_cast<int64_t>(commitCount);
            root["boundary_restores_enabled"] = static_cast<int64_t>(restoreCount);
            root["solve_time_ms"] = solveTimeMs;
            appendVMPlacementToJSON(root, vmPlacedGlobals, vmPlacedDetail);
            appendAbstractCFGStatsToJSON(root, abstractCFG.stats);
            root["ineligible_globals"] = static_cast<int64_t>(ineligGlobalCount);
            root["ineligible_allocas"] = static_cast<int64_t>(ineligAllocaCount);
            root["ineligible_ssa"] = static_cast<int64_t>(ineligSSACount);
            writeStatsJSON(StatsJsonOpt, std::move(root));
        }
        return preservedOnSkip();
    }

    // Step 7: Instrument IR (Pass F)
    CheckpointInstrumenter instrumenter(*F.getParent());
    unsigned inserted = instrumenter.instrumentFunction(F, solution, *abstractCFG.model,
                                                        *abstractCFG.model, *ctx.stateAnalysis);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Statistics summary
    {
        CommonStats common = makeCommonStats(totalExecutionTimeMs);
        common.regions = solution.r.size();
        common.regionBoundaries = solution.r.size();
        common.runtimeCallsInserted = inserted;
        printCommonStats(common);
    }

    PLOGI << "  --- MILP-specific ---";
    printAbstractCFGStats(abstractCFG.stats);
    printIneligibleStateStats(ineligGlobalCount, ineligAllocaCount, ineligSSACount);
    printMILPAllocationMode(coarseAllocation);
    printMILPProblemSizeStats(problemSizeStats);
    if (solution.solverStatus == SolverStatus::Optimal) {
        PLOGI << "  Optimal solution:                yes";
    } else {
        PLOGI << "  Optimal solution:                no (MIP gap: " << solution.mipGap << ")";
    }
    PLOGI << "  Boundary commits enabled:        " << commitCount;
    PLOGI << "  Boundary restores enabled:       " << restoreCount;
    PLOGI << "  VM-placed globals (of cand.):    " << vmPlacedGlobals << " / "
          << candidateGlobalCount;
    for (const auto &[name, blocks] : vmPlacedDetail)
        PLOGI << "      " << name << ": " << blocks << " block(s)";
    PLOGI << "  Solve time (ms):                 " << checkpoint::fmtDouble(solveTimeMs);

    if (!StatsJsonOpt.empty()) {
        CommonStats c = makeCommonStats(totalExecutionTimeMs);
        c.regions = solution.r.size();
        c.regionBoundaries = solution.r.size();
        c.runtimeCallsInserted = inserted;
        json::Object root = commonStatsToJSON(c);
        root["feasible"] = true;
        appendMILPAllocationModeToJSON(root, coarseAllocation);
        appendMILPProblemSizeStatsToJSON(root, problemSizeStats);
        if (solution.solverStatus == SolverStatus::Optimal) {
            root["optimal_solution"] = "yes";
        } else {
            root["optimal_solution"] =
                std::string("no (MIP gap: ") + std::to_string(solution.mipGap) + ")";
        }
        root["boundary_commits_enabled"] = static_cast<int64_t>(commitCount);
        root["boundary_restores_enabled"] = static_cast<int64_t>(restoreCount);
        root["solve_time_ms"] = solveTimeMs;
        appendVMPlacementToJSON(root, vmPlacedGlobals, vmPlacedDetail);
        appendAbstractCFGStatsToJSON(root, abstractCFG.stats);
        root["ineligible_globals"] = static_cast<int64_t>(ineligGlobalCount);
        root["ineligible_allocas"] = static_cast<int64_t>(ineligAllocaCount);
        root["ineligible_ssa"] = static_cast<int64_t>(ineligSSACount);
        writeStatsJSON(StatsJsonOpt, std::move(root));
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
