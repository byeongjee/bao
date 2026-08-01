#include "schematic/SchematicPass.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/PassStatistics.h"
#include "milp/CheckpointContext.h"
#include "schematic/CallFold.h"
#include "schematic/CallIsolation.h"
#include "schematic/CallSummary.h"
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

#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <map>
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

        rows.push_back({block->displayName(), &decision});
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

static CommonStats makeCommonStats(const Function &F, const CFGAnalysis &cfg,
                                   const SchematicStateAnalysis &state, double totalMs) {
    CommonStats c;
    c.passName = "SCHEMATIC";
    c.functionName = F.getName().str();
    c.basicBlocks = cfg.getBlocks().size();
    c.edges = cfg.getEdges().size();
    c.candidateGlobals = state.getCandidates().size();
    c.compilationTimeMs = totalMs;
    c.peakRSSKb = getPeakRSSKb();
    return c;
}

static void appendSolutionCounts(json::Object &root, const SchematicSolution &solution) {
    root["paths_analyzed"] = static_cast<int64_t>(solution.pathsAnalyzed);
    root["enabled_checkpoints"] = static_cast<int64_t>(solution.enabledCheckpoints.size());
    root["loop_decisions"] = static_cast<int64_t>(solution.loopDecisions.size());
}

static void writeInfeasibleStatsJson(const Function &F, const CFGAnalysis &cfg,
                                     const SchematicStateAnalysis &state,
                                     const SchematicSolution &solution,
                                     std::chrono::steady_clock::time_point totalStart) {
    if (StatsJsonOpt.empty())
        return;
    double totalMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - totalStart)
            .count();
    json::Object root = commonStatsToJSON(makeCommonStats(F, cfg, state, totalMs));
    root["feasible"] = false;
    root["infeasibility_reason"] = "energy capacity too small";
    appendSolutionCounts(root, solution);
    writeStatsJSON(StatsJsonOpt, std::move(root));
}

static void printSchematicStats(const Function &F, const CFGAnalysis &cfg,
                                const SchematicStateAnalysis &state,
                                const SchematicSolution &solution,
                                const SchematicInstrumenter &instrumenter, unsigned inserted,
                                double totalExecutionTimeMs) {
    CommonStats common = makeCommonStats(F, cfg, state, totalExecutionTimeMs);
    common.regions = solution.regions.size();
    common.regionBoundaries = instrumenter.boundaryCalls();
    common.runtimeCallsInserted = inserted;
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

        PLOGI << "    - " << edge.src->displayName() << " -> " << edge.dst->displayName() << " : "
              << mergedOrigins;
    }
}

// Fold each solved-callee summary onto its caller's isolated call sites. Faithful
// port of update_function_basic_blocks (schematic.py:95-131): a checkpoint-free
// callee folds transparently (DISABLED — the whole callee energy is baked into
// call_entry as one scalar); a callee that contains a checkpoint becomes a
// VIRTUAL wall with fixed entry/exit energies so caller regions stop at the call.
// Ref: §12.4. Must run before LoopAnalyzer (D5).
static bool foldCalleeSummaries(Function &F, SchematicGraph &graph, CFGAnalysis &cfg,
                                const SchematicParams &params,
                                const std::map<Function *, CallSummary> &summaries,
                                SchematicSolution &solution) {
    for (const IsolatedCall &ic : collectIsolatedCalls(F)) {
        auto sumIt = summaries.find(ic.callee);
        if (sumIt == summaries.end() || !sumIt->second.feasible) {
            // No usable callee summary (callee failed to solve or was skipped).
            // We cannot account for the callee's energy at this call site, so
            // leaving it unfolded would UNDER-count the caller's region energy and
            // risk an energy-unsafe region. Bail and skip this caller instead.
            PLOGE << "SCHEMATIC: no usable summary for callee '" << ic.callee->getName()
                  << "' called from '" << F.getName() << "'; skipping caller (cannot fold)";
            return false;
        }
        const CallSummary &S = sumIt->second;

        SchematicBlock *ceB = graph.getOrCreate(ic.entry);
        SchematicBlock *cxB = graph.getOrCreate(ic.exit);
        solution.functionCallBlocks.insert(ceB);
        solution.functionCallBlocks.insert(cxB);

        double c0 = cfg.getBlockInfo(ic.entry).energyCost; // raw CALL block energy
        double cx0 = cfg.getBlockInfo(ic.exit).energyCost; // empty exit block (~0)

        // Copy the callee's boundary allocations onto the call blocks (both
        // regimes): call_entry <- callee.first_bb, call_exit <- callee.last_bb.
        solution.blockAllocation[ceB] = std::make_shared<RegionAllocation>(S.sfAllocation);
        solution.blockAllocation[cxB] = std::make_shared<RegionAllocation>(S.efAllocation);
        solution.decidedPlacements[ceB] = S.sfPlacements;
        solution.decidedPlacements[cxB] = S.efPlacements;

        FoldedCallCosts fold =
            computeFoldedCallCosts(c0, cx0, S.sfEToLeave, S.sfELeft, S.efEToLeave, S.efELeft,
                                   params.capacity, S.checkpointInFunction);

        cfg.setBlockEnergyCost(ic.entry, fold.entryCost);
        cfg.setBlockEnergyCost(ic.exit, fold.exitCost);

        if (fold.regime == FoldRegime::Virtual) {
            // VIRTUAL: call_entry/call_exit become fixed barriers.
            BlockMetadata &ceMeta = solution.blockMeta[ceB];
            ceMeta.analyzed = true;
            ceMeta.E_left = fold.entryELeft;
            ceMeta.E_to_leave = fold.entryEToLeave;
            BlockMetadata &cxMeta = solution.blockMeta[cxB];
            cxMeta.analyzed = true;
            cxMeta.E_left = fold.exitELeft;
            cxMeta.E_to_leave = fold.exitEToLeave;
            setCheckpointState(solution, CFGEdge{ceB, cxB}, CheckpointState::Virtual);
        } else {
            // DISABLED: call blocks stay interior (analyzed=false); call_entry
            // carries the whole callee energy as one scalar.
            disableCheckpoint(solution, CFGEdge{ceB, cxB});
        }
    }
    return true;
}

// Capture this function's summary for folding into its callers. Reads the
// synthetic START_Func/END_Func boundary metadata (= reference cfg.first_bb /
// last_bb energy_to_leave/energy_left) and the has-checkpoint predicate. Must run
// before the FuncScopeGuard erases the synthetic nodes (§12.5).
static void captureCallSummary(CallSummary &out, const SchematicSolution &solution,
                               SchematicBlock *startFunc, SchematicBlock *endFunc) {
    auto metaOr = [&](SchematicBlock *b) -> BlockMetadata {
        auto it = solution.blockMeta.find(b);
        return it != solution.blockMeta.end() ? it->second : BlockMetadata{};
    };
    // A call summary may only carry module-scope GLOBALS: callee-local allocas are
    // dead in any caller and must not be folded onto a call site (they would
    // reference a foreign function's stack slot). Filter them out here.
    auto allocOr = [&](SchematicBlock *b) -> RegionAllocation {
        auto it = solution.blockAllocation.find(b);
        RegionAllocation a =
            (it != solution.blockAllocation.end() && it->second) ? *it->second : RegionAllocation{};
        RegionAllocation g;
        g.vmBytesUsed = a.vmBytesUsed;
        g.intervalEnergy = a.intervalEnergy;
        for (const auto &[v, va] : a.vars)
            if (llvm::isa<llvm::GlobalVariable>(v))
                g.vars[v] = va;
        for (const auto &[v, off] : a.vmOffsets)
            if (llvm::isa<llvm::GlobalVariable>(v))
                g.vmOffsets[v] = off;
        return g;
    };
    auto placeOr = [&](SchematicBlock *b) -> std::map<llvm::Value *, Placement> {
        std::map<llvm::Value *, Placement> g;
        auto it = solution.decidedPlacements.find(b);
        if (it != solution.decidedPlacements.end())
            for (const auto &[v, p] : it->second)
                if (llvm::isa<llvm::GlobalVariable>(v))
                    g[v] = p;
        return g;
    };

    BlockMetadata sf = metaOr(startFunc);
    BlockMetadata ef = metaOr(endFunc);
    out.feasible = true;
    out.sfEToLeave = sf.E_to_leave;
    out.sfELeft = sf.E_left;
    out.efEToLeave = ef.E_to_leave;
    out.efELeft = ef.E_left;
    out.sfAllocation = allocOr(startFunc);
    out.efAllocation = allocOr(endFunc);
    out.sfPlacements = placeOr(startFunc);
    out.efPlacements = placeOr(endFunc);
    out.checkpointInFunction = hasNonDisabledCheckpoint(solution);
}

bool SchematicPass::solveFunction(Function &F, FunctionAnalysisManager &AM,
                                  const SchematicParams &params, VMAddressTracker &sharedVMTracker,
                                  const std::map<Function *, CallSummary> &summaries,
                                  CallSummary &out, bool &mutatedIR) {
    const auto totalStart = std::chrono::steady_clock::now();

    // Obtain LLVM analyses.
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);

    // Hoist non-entry static allocas to the entry block. Must happen
    // BEFORE the checkpoint context is created: the context computes per-block
    // energy costs, and hoisting afterwards would leave the costs attributed to
    // the blocks the allocas were moved out of.
    BasicBlock &entryBB = F.getEntryBlock();
    for (BasicBlock &BB : F) {
        if (&BB == &entryBB)
            continue;
        for (auto it = BB.begin(); it != BB.end();) {
            auto *AI = dyn_cast<AllocaInst>(&*it++);
            if (AI && isa<ConstantInt>(AI->getArraySize())) {
                AI->moveBefore(entryBB, entryBB.getFirstInsertionPt());
                mutatedIR = true;
            }
        }
    }

    // Create base checkpoint context (estimator + CFG).
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(), "schematic pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            PLOGE << ctxResult.errorMessage;
        }
        return false;
    }
    auto &ctx = *ctxResult.context;

    // SCHEMATIC params + CLI overrides are resolved once by the driver.

    // Run SchematicStateAnalysis.
    SchematicStateAnalysis state(F, AA);
    if (state.hasAnalysisErrors()) {
        state.printAnalysisErrors(errs());
        PLOGE << "Skipping SCHEMATIC instrumentation for function " << F.getName()
              << " due to unresolved memory/call effects.";
        return false;
    }

    // Create the SchematicGraph that owns all SchematicBlock instances.
    SchematicGraph graph;
    graph.addCFGEdges(F);

    SchematicSolution solution;
    VMAddressTracker &vmTracker = sharedVMTracker;

    // Fold solved-callee summaries onto this function's isolated call
    // sites — BEFORE loop analysis (D5), so a call nested in a loop is costed
    // with the callee energy baked in. Ref: schematic.py:95-131,676-695. If a
    // callee summary is missing (callee failed to solve), skip this caller rather
    // than instrument it with an under-counted energy budget.
    if (!foldCalleeSummaries(F, graph, *ctx.cfg, params, summaries, solution))
        return false;

    // Load traces (required).
    if (SchematicTraceOpt.getValue().empty()) {
        PLOGE << "SCHEMATIC: no trace file given for " << F.getName() << " — traces are required";
        return false;
    }
    TraceLoader loader(F, LI, graph);
    std::optional<LoadedTraces> loadedTraces = loader.load(SchematicTraceOpt.getValue());
    if (!loadedTraces) {
        PLOGE << "SCHEMATIC: failed to load traces for " << F.getName();
        return false;
    }
    if (loadedTraces->functionPaths.empty()) {
        PLOGE << "SCHEMATIC: no function traces for " << F.getName() << " — traces are required";
        return false;
    }
    PLOGI << "SCHEMATIC: loaded traces for " << F.getName();

    // Loop analysis.
    LoopAnalyzer loopAnalyzer(LI, SE, *ctx.cfg, state, params, &vmTracker, graph);
    loopAnalyzer.setLoadedLoopTraces(loadedTraces->loopTraces);

    if (!loopAnalyzer.analyzeLoops(solution)) {
        PLOGE << "SCHEMATIC: loop analysis failed for " << F.getName() << " — aborting";
        return false;
    }

    std::vector<EnumeratedPath> paths = loadedTraces->functionPaths;

    // Create synthetic START_Func/END_Func boundary blocks for function traces.
    // These match the Python reference's %START_ / %END_ synthetic nodes that
    // are prepended/appended to every function trace (trace.py:288-289).
    SchematicBlock *startFunc = graph.createSynthetic(kStartFuncName.str());
    SchematicBlock *endFunc = graph.createSynthetic(kEndFuncName.str());

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

    // Analyze each path.
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
            writeInfeasibleStatsJson(F, *ctx.cfg, state, solution, totalStart);
            return false;
        }
    }

    // Analyze uncovered blocks (Python: find_and_analyse_not_fixed_paths).
    {
        std::string uncoveredError;
        if (!findAndAnalyzeNotFixedPaths(*ctx.cfg, solution, state, params, &vmTracker, LI,
                                         /*loopScope=*/nullptr, graph, uncoveredError)) {
            PLOGE << "SCHEMATIC infeasible: energy capacity too small for function '" << F.getName()
                  << "', uncovered blocks: " << uncoveredError;
            writeInfeasibleStatsJson(F, *ctx.cfg, state, solution, totalStart);
            return false;
        }
    }

    // Resolve remaining potential edges (reference: schematic.py:468-502).
    removePotentialCheckpointsBetweenFixedBBs(*ctx.cfg, solution, state, params, LI, graph);

    // Collect statistics.
    for (const auto &region : solution.regions) {
        for (const auto &[gv, va] : region.allocation.vars) {
            if (va.placement == Placement::VM)
                solution.totalVmVariables++;
            else
                solution.totalNvmVariables++;
        }
    }

    // Instrument.
    SchematicInstrumenter instrumenter(*F.getParent(), params.addDebugMarkers);
    unsigned inserted = instrumenter.instrumentFunction(F, solution, state);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Print statistics.
    printSchematicStats(F, *ctx.cfg, state, solution, instrumenter, inserted, totalExecutionTimeMs);

    if (!StatsJsonOpt.empty()) {
        CommonStats c = makeCommonStats(F, *ctx.cfg, state, totalExecutionTimeMs);
        c.regions = solution.regions.size();
        c.regionBoundaries = instrumenter.boundaryCalls();
        c.runtimeCallsInserted = inserted;
        json::Object root = commonStatsToJSON(c);
        root["feasible"] = true;
        appendSolutionCounts(root, solution);
        root["loop_decision_details"] = buildLoopDecisionDetails(solution);
        writeStatsJSON(StatsJsonOpt, std::move(root));
    }

    // Capture the callee summary while START_Func/END_Func still live (the
    // FuncScopeGuard erases them when this function returns).
    captureCallSummary(out, solution, startFunc, endFunc);
    return true;
}

PreservedAnalyses SchematicPass::run(Module &M, ModuleAnalysisManager &MAM) {
    initLogging();

    // Parse SCHEMATIC params once for the whole module.
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

    FunctionAnalysisManager &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

    // ONE program-wide VM address tracker: globals get stable VM addresses and
    // the VM capacity budget is enforced across all functions (ref: the single
    // MemoryAllocator/top_address in schematic.py).
    VMAddressTracker vmTracker;

    // Bottom-up over the call graph (callees before callers). scc_begin yields
    // SCCs in reverse-topological order, so leaf functions are solved first and
    // each callee summary exists before its caller is solved (ref:
    // schematic.py:676-695). Recursion is rejected by the schematic-isolate pass.
    CallGraph CG(M);
    std::map<Function *, CallSummary> summaries;
    bool changed = false;
    for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
        for (CallGraphNode *node : *I) {
            Function *F = node->getFunction();
            if (!F || F->isDeclaration())
                continue;
            if (isBenchmarkInfrastructureFunction(F->getName())) {
                PLOGI << "SCHEMATIC: skipping benchmark infrastructure function " << F->getName();
                continue;
            }
            CallSummary summary;
            // Track IR mutation separately from solve success: a failed solve may
            // still have hoisted allocas, and reporting PreservedAnalyses::all()
            // after mutating IR would leave stale analyses behind.
            bool mutatedIR = false;
            if (solveFunction(*F, FAM, params, vmTracker, summaries, summary, mutatedIR)) {
                summaries[F] = std::move(summary);
                changed = true;
            }
            changed |= mutatedIR;
        }
    }

    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace checkpoint
