#include "schematic/SchematicPass.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/PassStatistics.h"
#include "milp/CheckpointContext.h"
#include "schematic/LoopAnalyzer.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/SchematicBlock.h"
#include "schematic/SchematicInstrumenter.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"
#include "schematic/TraceAnalyzer.h"
#include "schematic/TraceLoader.h"

#include "common/FunctionFilters.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <set>

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> SchematicConfigOpt;
extern cl::opt<std::string> SchematicTraceOpt;
extern cl::opt<bool> AddDebugMarkersOpt;
extern cl::opt<bool> ForceCheckpointOnIncompatibleLoopsOpt;
extern cl::opt<bool> RecomputeEnergyAfterNewCheckpointOpt;

namespace checkpoint {

static std::string getSchematicBlockName(const SchematicBlock *block) {
    if (const llvm::BasicBlock *BB = block->getLLVMBlock())
        return BB->getName().str();
    return block->getName().str();
}

static json::Array buildLoopDecisionDetails(const SchematicSolution &solution) {
    struct LoopDecisionRow {
        std::string headerName;
        const LoopCheckpointDecision *decision;
    };

    std::vector<LoopDecisionRow> rows;
    rows.reserve(solution.loopDecisions.size());
    for (const auto &[block, decision] : solution.loopDecisions) {
        if (!block)
            continue;

        std::string headerName;
        if (llvm::BasicBlock *BB = block->getLLVMBlock()) {
            headerName = BB->getName().str();
        } else {
            headerName = block->getName().str();
        }

        rows.push_back({headerName, &decision});
    }

    std::sort(rows.begin(), rows.end(), [](const LoopDecisionRow &lhs, const LoopDecisionRow &rhs) {
        return lhs.headerName < rhs.headerName;
    });

    json::Array details;
    details.reserve(rows.size());
    for (const auto &row : rows) {
        json::Object item;
        item["loop_header"] = row.headerName;
        item["mandatory_backedge"] = row.decision->mandatoryBackEdge;
        item["loop_fits_entirely"] = row.decision->loopFitsEntirely;
        item["num_iterations_per_charge"] =
            static_cast<int64_t>(row.decision->numIterationsPerCharge);
        item["e_loop"] = row.decision->E_loop;
        item["body_path_count"] = static_cast<int64_t>(row.decision->bodyPathCount);
        item["had_enabled_checkpoints"] = row.decision->hadEnabledCheckpoints;
        item["convergence_applied"] = row.decision->convergenceApplied;
        item["convergence_iterations"] = static_cast<int64_t>(row.decision->convergenceIterations);
        item["initial_start_e_to_leave"] = row.decision->initialStartEToLeave;
        item["initial_end_e_to_leave"] = row.decision->initialEndEToLeave;
        item["initial_e_loop"] = row.decision->initialELoop;
        item["initial_available_energy"] = row.decision->initialAvailableEnergy;
        item["initial_raw_num_iterations"] =
            static_cast<int64_t>(row.decision->initialRawNumIterations);
        item["final_start_e_to_leave"] = row.decision->finalStartEToLeave;
        item["final_end_e_to_leave"] = row.decision->finalEndEToLeave;
        item["final_available_energy"] = row.decision->finalAvailableEnergy;
        item["final_raw_num_iterations"] =
            static_cast<int64_t>(row.decision->finalRawNumIterations);
        item["body_vm_bytes_used"] = static_cast<int64_t>(row.decision->bodyAllocation.vmBytesUsed);
        details.emplace_back(std::move(item));
    }

    return details;
}

static void printSchematicStats(const Function &F, const CFGAnalysis &cfg,
                                const SchematicStateAnalysis &state,
                                const SchematicSolution &solution,
                                const SchematicInstrumenter &instrumenter, unsigned inserted,
                                double totalExecutionTimeMs) {
    CommonStats common;
    common.passName = "SCHEMATIC";
    common.functionName = F.getName().str();
    common.basicBlocks = cfg.getBlocks().size();
    common.edges = cfg.getEdges().size();
    common.candidateGlobals = state.getCandidates().size();
    common.regions = solution.regions.size();
    common.regionBoundaries = instrumenter.boundaryCalls();
    common.runtimeCallsInserted = inserted;
    common.compilationTimeMs = totalExecutionTimeMs;
    common.peakRSSKb = getPeakRSSKb();
    printCommonStats(common);

    PLOGI << "  --- SCHEMATIC-specific ---";
    PLOGI << "  Paths analyzed:                  " << solution.pathsAnalyzed;
    PLOGI << "  Enabled checkpoints:             " << solution.enabledCheckpoints.size();
    PLOGI << "  Loop decisions:                  " << solution.loopDecisions.size();
    PLOGI << "  Boundary calls inserted:         " << instrumenter.boundaryCalls();
    PLOGI << "  Store mem calls inserted:        " << instrumenter.storeMemCalls();
    PLOGI << "  Restore mem calls inserted:      " << instrumenter.restoreMemCalls();
    PLOGI << "  Trace-guided:                    yes";

    if (solution.checkpointOrigins.empty())
        return;

    PLOGI << "  Checkpoint provenance:";
    for (const auto &[edge, origins] : solution.checkpointOrigins) {
        std::string mergedOrigins;
        for (size_t i = 0; i < origins.size(); ++i) {
            if (i > 0)
                mergedOrigins += ", ";
            mergedOrigins += origins[i];
        }

        PLOGI << "    - " << getSchematicBlockName(edge.src) << " -> "
              << getSchematicBlockName(edge.dst) << " : " << mergedOrigins;
    }
}

PreservedAnalyses SchematicPass::run(Function &F, FunctionAnalysisManager &AM) {
    initLogging();
    const auto totalStart = std::chrono::steady_clock::now();

    if (isBenchmarkInfrastructureFunction(F.getName())) {
        PLOGI << "SCHEMATIC: skipping benchmark infrastructure function " << F.getName();
        return PreservedAnalyses::all();
    }

    // Step 1: Obtain LLVM analyses.
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);

    // Step 2: Create base checkpoint context (estimator + CFG).
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(), "schematic pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip())
            PLOGE << ctxResult.errorMessage;
        return PreservedAnalyses::all();
    }
    auto &ctx = *ctxResult.context;

    // Step 3: Parse SCHEMATIC params.
    auto paramsOpt = parseSchematicParams(SchematicConfigOpt.getValue());
    if (!paramsOpt) {
        PLOGE << "Error: Failed to parse SCHEMATIC config: " << SchematicConfigOpt.getValue();
        return PreservedAnalyses::all();
    }
    SchematicParams params = *paramsOpt;

    // Override flags from CLI.
    if (AddDebugMarkersOpt.getValue())
        params.addDebugMarkers = true;
    if (ForceCheckpointOnIncompatibleLoopsOpt.getValue())
        params.forceCheckpointOnIncompatibleLoops = true;
    if (RecomputeEnergyAfterNewCheckpointOpt.getValue())
        params.recomputeEnergyAfterNewCheckpoint = true;

    // Step 4: Hoist non-entry static allocas to the entry block.
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

    // Step 5: Run SchematicStateAnalysis.
    SchematicStateAnalysis state(F, AA, *ctx.cfg);
    if (state.hasAnalysisErrors()) {
        state.printAnalysisErrors(errs());
        PLOGE << "Skipping SCHEMATIC instrumentation for function " << F.getName()
              << " due to unresolved memory/call effects.";
        return PreservedAnalyses::all();
    }

    // Create the SchematicGraph that owns all SchematicBlock instances.
    SchematicGraph graph;
    graph.addCFGEdges(F);

    SchematicSolution solution;
    VMAddressTracker vmTracker;

    // Step 6: Load traces (optional).
    std::optional<LoadedTraces> loadedTraces;
    if (!SchematicTraceOpt.getValue().empty()) {
        TraceLoader loader(F, LI, graph);
        loadedTraces = loader.load(SchematicTraceOpt.getValue());
        if (!loadedTraces) {
            PLOGE << "SCHEMATIC: failed to load traces for " << F.getName();
            return PreservedAnalyses::all();
        }
        PLOGI << "SCHEMATIC: loaded traces for " << F.getName();
    }

    // Step 7: Loop analysis.
    LoopAnalyzer loopAnalyzer(LI, SE, *ctx.cfg, state, params, &vmTracker, graph);
    if (loadedTraces)
        loopAnalyzer.setLoadedLoopTraces(loadedTraces->loopTraces);

    if (!loopAnalyzer.analyzeLoops(solution)) {
        PLOGE << "SCHEMATIC: loop analysis failed for " << F.getName() << " — aborting";
        return PreservedAnalyses::all();
    }

    // Step 8: Get paths from traces (required).
    if (!loadedTraces || loadedTraces->functionPaths.empty()) {
        PLOGE << "SCHEMATIC: no traces loaded for " << F.getName() << " — traces are required";
        return PreservedAnalyses::all();
    }
    std::vector<EnumeratedPath> paths = loadedTraces->functionPaths;

    // Create synthetic START_Func/END_Func boundary blocks for function traces.
    // These match the Python reference's %START_ / %END_ synthetic nodes that
    // are prepended/appended to every function trace (trace.py:288-289).
    SchematicBlock *startFunc = graph.createSynthetic("START_Func");
    SchematicBlock *endFunc = graph.createSynthetic("END_Func");

    // Mark synthetic boundaries as analyzed with default energy values
    // (same pattern as LoopAnalyzer's START_Loop/END_Loop).
    solution.blockMeta[startFunc].E_left =
        params.capacity - params.E_pro - params.N_reg * params.regRestoreEnergy;
    solution.blockMeta[startFunc].analyzed = true;
    solution.decidedPlacements[startFunc] = {};

    solution.blockMeta[endFunc].E_to_leave = params.E_epi + params.N_reg * params.regStoreEnergy;
    solution.blockMeta[endFunc].analyzed = true;
    solution.decidedPlacements[endFunc] = {};

    // Cleanup for synthetic blocks from solution maps on exit.
    // Graph owns the blocks, so no delete needed.
    auto cleanupFunc = [&]() {
        solution.blockMeta.erase(startFunc);
        solution.blockMeta.erase(endFunc);
        solution.decidedPlacements.erase(startFunc);
        solution.decidedPlacements.erase(endFunc);
        solution.blockAllocation.erase(startFunc);
        solution.blockAllocation.erase(endFunc);
    };
    struct FuncScopeGuard {
        std::function<void()> fn;
        ~FuncScopeGuard() { fn(); }
    } funcGuard{cleanupFunc};

    // Step 9: Analyze each path.
    for (const auto &ep : paths) {
        solution.pathsAnalyzed++;
        // Prepend/append synthetic boundary blocks (ref: trace.py:288-289).
        std::vector<SchematicBlock *> tracePath;
        tracePath.reserve(ep.blocks.size() + 2);
        tracePath.push_back(startFunc);
        tracePath.insert(tracePath.end(), ep.blocks.begin(), ep.blocks.end());
        tracePath.push_back(endFunc);

        // Add trace edges so SchematicBlock predecessors/successors are populated.
        graph.addTraceEdges(tracePath);

        std::string traceError;
        if (!analyzeTrace(tracePath, solution, state, *ctx.cfg, params, &vmTracker, LI,
                          /*loopScope=*/nullptr, traceError)) {
            PLOGE << "SCHEMATIC infeasible: energy capacity too small for function '" << F.getName()
                  << "', path #" << solution.pathsAnalyzed << ": " << traceError;
            if (!StatsJsonOpt.empty()) {
                const auto totalEnd = std::chrono::steady_clock::now();
                double totalMs =
                    std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
                CommonStats c;
                c.passName = "SCHEMATIC";
                c.functionName = F.getName().str();
                c.basicBlocks = ctx.cfg->getBlocks().size();
                c.edges = ctx.cfg->getEdges().size();
                c.candidateGlobals = state.getCandidates().size();
                c.compilationTimeMs = totalMs;
                c.peakRSSKb = getPeakRSSKb();
                json::Object root = commonStatsToJSON(c);
                root["feasible"] = false;
                root["infeasibility_reason"] = "energy capacity too small";
                root["paths_analyzed"] = static_cast<int64_t>(solution.pathsAnalyzed);
                root["enabled_checkpoints"] =
                    static_cast<int64_t>(solution.enabledCheckpoints.size());
                root["loop_decisions"] = static_cast<int64_t>(solution.loopDecisions.size());
                writeStatsJSON(StatsJsonOpt, std::move(root));
            }
            return PreservedAnalyses::all();
        }
    }

    // Step 9b: Analyze uncovered blocks (Python: find_and_analyse_not_fixed_paths).
    {
        std::string uncoveredError;
        if (!findAndAnalyzeNotFixedPaths(*ctx.cfg, solution, state, params, &vmTracker, LI,
                                         /*loopScope=*/nullptr, graph, uncoveredError)) {
            PLOGE << "SCHEMATIC infeasible: energy capacity too small for function '" << F.getName()
                  << "', uncovered blocks: " << uncoveredError;
            if (!StatsJsonOpt.empty()) {
                const auto totalEnd = std::chrono::steady_clock::now();
                double totalMs =
                    std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
                CommonStats c;
                c.passName = "SCHEMATIC";
                c.functionName = F.getName().str();
                c.basicBlocks = ctx.cfg->getBlocks().size();
                c.edges = ctx.cfg->getEdges().size();
                c.candidateGlobals = state.getCandidates().size();
                c.compilationTimeMs = totalMs;
                c.peakRSSKb = getPeakRSSKb();
                json::Object root = commonStatsToJSON(c);
                root["feasible"] = false;
                root["infeasibility_reason"] = "energy capacity too small";
                root["paths_analyzed"] = static_cast<int64_t>(solution.pathsAnalyzed);
                root["enabled_checkpoints"] =
                    static_cast<int64_t>(solution.enabledCheckpoints.size());
                root["loop_decisions"] = static_cast<int64_t>(solution.loopDecisions.size());
                writeStatsJSON(StatsJsonOpt, std::move(root));
            }
            return PreservedAnalyses::all();
        }
    }

    // Step 10: Resolve remaining potential edges (reference: schematic.py:468-502).
    removePotentialCheckpointsBetweenFixedBBs(*ctx.cfg, solution, state, params, LI, graph);

    // Step 11: Collect statistics.
    for (const auto &region : solution.regions) {
        for (const auto &[gv, va] : region.allocation.vars) {
            if (va.placement == Placement::VM)
                solution.totalVmVariables++;
            else
                solution.totalNvmVariables++;
        }
    }

    // Step 12: Instrument.
    SchematicInstrumenter instrumenter(*F.getParent(), params.addDebugMarkers);
    unsigned inserted = instrumenter.instrumentFunction(F, solution, state);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Step 13: Print statistics.
    printSchematicStats(F, *ctx.cfg, state, solution, instrumenter, inserted, totalExecutionTimeMs);

    if (!StatsJsonOpt.empty()) {
        CommonStats c;
        c.passName = "SCHEMATIC";
        c.functionName = F.getName().str();
        c.basicBlocks = ctx.cfg->getBlocks().size();
        c.edges = ctx.cfg->getEdges().size();
        c.candidateGlobals = state.getCandidates().size();
        c.regions = solution.regions.size();
        c.regionBoundaries = instrumenter.boundaryCalls();
        c.runtimeCallsInserted = inserted;
        c.compilationTimeMs = totalExecutionTimeMs;
        c.peakRSSKb = getPeakRSSKb();
        json::Object root = commonStatsToJSON(c);
        root["feasible"] = true;
        root["paths_analyzed"] = static_cast<int64_t>(solution.pathsAnalyzed);
        root["enabled_checkpoints"] = static_cast<int64_t>(solution.enabledCheckpoints.size());
        root["loop_decisions"] = static_cast<int64_t>(solution.loopDecisions.size());
        root["loop_decision_details"] = buildLoopDecisionDetails(solution);
        writeStatsJSON(StatsJsonOpt, std::move(root));
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
