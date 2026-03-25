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

namespace checkpoint {

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
        if (loadedTraces)
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
    {
        CommonStats common;
        common.passName = "SCHEMATIC";
        common.functionName = F.getName().str();
        common.basicBlocks = ctx.cfg->getBlocks().size();
        common.edges = ctx.cfg->getEdges().size();
        common.candidateGlobals = state.getCandidates().size();
        common.regions = solution.regions.size();
        common.regionBoundaries = instrumenter.boundaryCalls();
        common.runtimeCallsInserted = inserted;
        common.compilationTimeMs = totalExecutionTimeMs;
        common.peakRSSKb = getPeakRSSKb();
        printCommonStats(common);
    }

    PLOGI << "  --- SCHEMATIC-specific ---";
    PLOGI << "  Paths analyzed:                  " << solution.pathsAnalyzed;
    PLOGI << "  Enabled checkpoints:             " << solution.enabledCheckpoints.size();
    PLOGI << "  Loop decisions:                  " << solution.loopDecisions.size();
    PLOGI << "  Boundary calls inserted:         " << instrumenter.boundaryCalls();
    PLOGI << "  Store mem calls inserted:        " << instrumenter.storeMemCalls();
    PLOGI << "  Restore mem calls inserted:      " << instrumenter.restoreMemCalls();
    PLOGI << "  Trace-guided:                    yes";
    if (!solution.checkpointOrigins.empty()) {
        PLOGI << "  Checkpoint provenance:";
        for (const auto &[edge, origins] : solution.checkpointOrigins) {
            auto edgeName = [](SchematicBlock *block) -> std::string {
                return block->getLLVMBlock() ? block->getLLVMBlock()->getName().str()
                                             : block->getName().str();
            };
            std::string mergedOrigins;
            for (size_t i = 0; i < origins.size(); ++i) {
                if (i > 0)
                    mergedOrigins += ", ";
                mergedOrigins += origins[i];
            }
            PLOGI << "    - " << edgeName(edge.src) << " -> " << edgeName(edge.dst) << " : "
                  << mergedOrigins;
        }
    }

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
        writeStatsJSON(StatsJsonOpt, std::move(root));
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
