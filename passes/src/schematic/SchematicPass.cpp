#include "schematic/SchematicPass.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/PassStatistics.h"
#include "milp/CheckpointContext.h"
#include "schematic/IntervalAllocator.h"
#include "schematic/LoopAnalyzer.h"
#include "schematic/RCGSolver.h"
#include "schematic/SchematicInstrumenter.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"
#include "schematic/TraceLoader.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <deque>
#include <set>

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> SchematicConfigOpt;
extern cl::opt<std::string> SchematicTraceOpt;
extern cl::opt<bool> AddDebugMarkersOpt;

namespace checkpoint {

PreservedAnalyses SchematicPass::run(Function &F, FunctionAnalysisManager &AM) {
    initLogging();
    const auto totalStart = std::chrono::steady_clock::now();

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

    // Override debug markers from CLI.
    if (AddDebugMarkersOpt.getValue())
        params.addDebugMarkers = true;

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

    SchematicSolution solution;
    VMAddressTracker vmTracker;

    // Step 6: Load traces (optional).
    std::optional<LoadedTraces> loadedTraces;
    if (!SchematicTraceOpt.getValue().empty()) {
        TraceLoader loader(F, LI);
        loadedTraces = loader.load(SchematicTraceOpt.getValue());
        if (loadedTraces)
            PLOGI << "SCHEMATIC: loaded traces for " << F.getName();
    }

    // Step 7: Loop analysis.
    LoopAnalyzer loopAnalyzer(LI, SE, *ctx.cfg, state, params, &vmTracker);
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

    // Energy helper lambdas (used by propagateEnergy and interval processing).
    auto getBlockExecEnergy = [&](BasicBlock *BB) -> double {
        double E = ctx.cfg->getBlockInfo(BB).energyCost;
        auto allocIt = solution.decidedPlacements.find(BB);
        if (allocIt != solution.decidedPlacements.end()) {
            for (const auto &[gv, place] : allocIt->second) {
                if (place != Placement::VM)
                    continue;
                unsigned loads = state.getLoadCount(BB, gv);
                unsigned stores = state.getStoreCount(BB, gv);
                E -= params.nvmAccessPenalty * (loads + stores);
            }
        }
        return E;
    };

    auto getVarRestoreCost = [&](BasicBlock *BB) -> double {
        double cost = 0.0;
        auto allocIt = solution.decidedPlacements.find(BB);
        if (allocIt != solution.decidedPlacements.end()) {
            for (const auto &[gv, place] : allocIt->second) {
                if (place != Placement::VM)
                    continue;
                cost += params.memRestoreEnergyPerByte * state.getVarSizeBytes(gv);
            }
        }
        return cost;
    };

    auto getVarSaveCost = [&](BasicBlock *BB) -> double {
        double cost = 0.0;
        auto allocIt = solution.decidedPlacements.find(BB);
        if (allocIt != solution.decidedPlacements.end()) {
            for (const auto &[gv, place] : allocIt->second) {
                if (place != Placement::VM)
                    continue;
                cost += params.memStoreEnergyPerByte * state.getVarSizeBytes(gv);
            }
        }
        return cost;
    };

    // CFG-based energy propagation (reference: cfg_modification.py:171-317).
    // Propagates E_left forward and E_to_leave backward through disabled
    // checkpoint chains. Order: Phase 3 (seed checkpoint entries) → forward → backward.
    auto propagateEnergy = [&]() {
        // Phase 3: Initialize E_left at checkpoint entry points (Fix 4: before forward prop).
        for (const auto &ckpt : solution.enabledCheckpoints) {
            BasicBlock *dstBB = ckpt.dst;
            auto dstIt = solution.blockMeta.find(dstBB);
            if (dstIt == solution.blockMeta.end() || !dstIt->second.analyzed)
                continue;
            double restoreE =
                params.E_pro + params.N_reg * params.regRestoreEnergy + getVarRestoreCost(dstBB);
            double dstExecEnergy = getBlockExecEnergy(dstBB);
            double newELeft = params.capacity - restoreE - dstExecEnergy;
            if (newELeft < dstIt->second.E_left)
                solution.blockMeta[dstBB].E_left = newELeft;
        }

        // Forward propagation of E_left through disabled edges.
        std::set<BasicBlock *> fwdVisitedLoopHeaders;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &[src, dst] : ctx.cfg->getEdges()) {
                auto *srcBB = const_cast<BasicBlock *>(src);
                auto *dstBB = const_cast<BasicBlock *>(dst);
                CFGEdge edge{srcBB, dstBB};

                if (solution.enabledCheckpoints.count(edge))
                    continue;

                // Skip loop back-edges.
                if (Loop *L = LI.getLoopFor(dstBB)) {
                    if (dstBB == L->getHeader() && L->contains(srcBB))
                        continue;
                }

                auto srcIt = solution.blockMeta.find(srcBB);
                auto dstIt = solution.blockMeta.find(dstBB);
                if (srcIt == solution.blockMeta.end() || !srcIt->second.analyzed)
                    continue;
                if (dstIt == solution.blockMeta.end() || !dstIt->second.analyzed)
                    continue;

                double dstExecEnergy = getBlockExecEnergy(dstBB);

                // Loop-aware scaling (reference: cfg_modification.py:293-295).
                // Any block in a loop triggers scaling (not just headers).
                if (Loop *L = LI.getLoopFor(dstBB)) {
                    BasicBlock *header = L->getHeader();
                    if (auto loopIt = solution.loopDecisions.find(header);
                        loopIt != solution.loopDecisions.end() &&
                        !fwdVisitedLoopHeaders.count(header)) {
                        fwdVisitedLoopHeaders.insert(header);
                        unsigned nbIter = loopIt->second.numIterationsPerCharge;
                        if (nbIter > 1)
                            dstExecEnergy += (nbIter - 1) * loopIt->second.E_loop;
                    }
                }

                double newELeft = srcIt->second.E_left - dstExecEnergy;
                if (newELeft < dstIt->second.E_left) {
                    solution.blockMeta[dstBB].E_left = newELeft;
                    changed = true;
                }
            }
        }

        // Backward propagation of E_to_leave through disabled edges.
        std::set<BasicBlock *> bwdVisitedLoopHeaders;
        changed = true;
        while (changed) {
            changed = false;
            for (const auto &[src, dst] : ctx.cfg->getEdges()) {
                auto *srcBB = const_cast<BasicBlock *>(src);
                auto *dstBB = const_cast<BasicBlock *>(dst);
                CFGEdge edge{srcBB, dstBB};

                // At enabled checkpoints, E_to_leave includes save + exec cost.
                if (solution.enabledCheckpoints.count(edge)) {
                    double saveE =
                        params.E_epi + params.N_reg * params.regStoreEnergy + getVarSaveCost(srcBB);
                    saveE += getBlockExecEnergy(srcBB);
                    if (saveE > solution.blockMeta[srcBB].E_to_leave) {
                        solution.blockMeta[srcBB].E_to_leave = saveE;
                        changed = true;
                    }
                    continue;
                }

                // Skip loop back-edges.
                if (Loop *L = LI.getLoopFor(dstBB)) {
                    if (dstBB == L->getHeader() && L->contains(srcBB))
                        continue;
                }

                auto srcIt = solution.blockMeta.find(srcBB);
                auto dstIt = solution.blockMeta.find(dstBB);
                if (srcIt == solution.blockMeta.end() || !srcIt->second.analyzed)
                    continue;
                if (dstIt == solution.blockMeta.end() || !dstIt->second.analyzed)
                    continue;

                double srcExecEnergy = getBlockExecEnergy(srcBB);

                // Loop-aware scaling (reference: cfg_modification.py:232-234).
                // Any block in a loop triggers scaling (not just headers).
                if (Loop *L = LI.getLoopFor(srcBB)) {
                    BasicBlock *header = L->getHeader();
                    if (auto loopIt = solution.loopDecisions.find(header);
                        loopIt != solution.loopDecisions.end() &&
                        !bwdVisitedLoopHeaders.count(header)) {
                        bwdVisitedLoopHeaders.insert(header);
                        unsigned nbIter = loopIt->second.numIterationsPerCharge;
                        if (nbIter > 1)
                            srcExecEnergy += (nbIter - 1) * loopIt->second.E_loop;
                    }
                }

                double newEToLeave = srcExecEnergy + dstIt->second.E_to_leave;
                if (newEToLeave > srcIt->second.E_to_leave) {
                    solution.blockMeta[srcBB].E_to_leave = newEToLeave;
                    changed = true;
                }
            }
        }
    };

    // Helper: update solution from RCG interval results.
    // Only sets analyzed flag, placements, and regions. E_left/E_to_leave are
    // handled entirely by propagateEnergy() (reference delegates to
    // propagate_energy_left / propagate_energy_to_leave).
    auto updateSolutionFromIntervals = [&](const RCGResult &result) {
        for (const auto &ckpt : result.selectedCheckpoints)
            solution.enabledCheckpoints.insert(ckpt);

        for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
            const auto &blocks = result.intervalBlocks[i];
            const auto &alloc = result.allocations[i];

            for (BasicBlock *BB : blocks) {
                auto &meta = solution.blockMeta[BB];
                meta.analyzed = true;
                for (const auto &[gv, place] : alloc.placement)
                    solution.decidedPlacements[BB][gv] = place;
            }

            solution.regions.push_back({blocks, alloc});
        }
    };

    // Step 9: Analyze each path.
    for (const auto &ep : paths) {
        solution.pathsAnalyzed++;

        // Skip if all blocks already analyzed.
        bool allAnalyzed = true;
        for (llvm::BasicBlock *BB : ep.blocks) {
            auto it = solution.blockMeta.find(BB);
            if (it == solution.blockMeta.end() || !it->second.analyzed) {
                allAnalyzed = false;
                break;
            }
        }
        if (allAnalyzed)
            continue;

        // Extract contiguous unanalyzed segments.
        std::vector<std::vector<llvm::BasicBlock *>> segments;
        std::vector<llvm::BasicBlock *> currentSeg;
        std::vector<llvm::BasicBlock *> startBoundaries;
        std::vector<llvm::BasicBlock *> endBoundaries;

        for (unsigned i = 0; i < ep.blocks.size(); ++i) {
            llvm::BasicBlock *BB = ep.blocks[i];
            auto metaIt = solution.blockMeta.find(BB);
            bool isAnalyzed = metaIt != solution.blockMeta.end() && metaIt->second.analyzed;

            if (!isAnalyzed) {
                if (currentSeg.empty()) {
                    // Record start boundary (previous analyzed block).
                    llvm::BasicBlock *startBound = nullptr;
                    if (i > 0) {
                        auto prevMeta = solution.blockMeta.find(ep.blocks[i - 1]);
                        if (prevMeta != solution.blockMeta.end() && prevMeta->second.analyzed)
                            startBound = ep.blocks[i - 1];
                    }
                    startBoundaries.push_back(startBound);
                }
                currentSeg.push_back(BB);
            } else {
                if (!currentSeg.empty()) {
                    endBoundaries.push_back(BB);
                    segments.push_back(std::move(currentSeg));
                    currentSeg.clear();
                }
            }
        }
        if (!currentSeg.empty()) {
            endBoundaries.push_back(nullptr);
            segments.push_back(std::move(currentSeg));
        }

        // Solve each segment with RCG.
        for (unsigned s = 0; s < segments.size(); ++s) {
            llvm::BasicBlock *startBound =
                s < startBoundaries.size() ? startBoundaries[s] : nullptr;
            llvm::BasicBlock *endBound = s < endBoundaries.size() ? endBoundaries[s] : nullptr;

            RCGSolver solver(segments[s], state, *ctx.cfg, params, solution.blockMeta,
                             solution.decidedPlacements, startBound, endBound, &vmTracker);
            RCGResult result = solver.solve();

            if (!result.feasible) {
                PLOGE << "SCHEMATIC infeasible: energy capacity too small for function '"
                      << F.getName() << "', path #" << solution.pathsAnalyzed << ": "
                      << result.errorMessage;
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

            updateSolutionFromIntervals(result);
        }

        // Propagate energy after each path so subsequent paths see updated values.
        propagateEnergy();
    }

    // Propagate energy before Step 9b so boundary blocks have correct values.
    propagateEnergy();

    // Step 9b: Analyze uncovered blocks (Python: find_and_analyse_not_fixed_paths).
    // For each block not yet analyzed, build a synthetic path:
    //   [analyzed predecessor] → unfixed blocks (greedy walk) → [analyzed successor]
    // and run the same RCG analysis on it.
    for (const BasicBlock *constBB : ctx.cfg->getBlocks()) {
        auto *BB = const_cast<BasicBlock *>(constBB);
        auto metaIt = solution.blockMeta.find(BB);
        if (metaIt != solution.blockMeta.end() && metaIt->second.analyzed)
            continue;

        // Build synthetic path: start with an analyzed predecessor.
        std::vector<BasicBlock *> synPath;

        // Find an analyzed predecessor as the start boundary.
        BasicBlock *startBound = nullptr;
        for (BasicBlock *pred : predecessors(BB)) {
            auto pMeta = solution.blockMeta.find(pred);
            if (pMeta != solution.blockMeta.end() && pMeta->second.analyzed) {
                startBound = pred;
                break;
            }
        }

        // Walk forward through unanalyzed blocks.
        std::set<BasicBlock *> visited;
        std::deque<BasicBlock *> toVisit;
        toVisit.push_back(BB);
        while (!toVisit.empty()) {
            BasicBlock *cur = toVisit.back();
            toVisit.pop_back();
            if (visited.count(cur))
                continue;
            auto curMeta = solution.blockMeta.find(cur);
            if (curMeta != solution.blockMeta.end() && curMeta->second.analyzed)
                continue;
            visited.insert(cur);
            synPath.push_back(cur);
            // Follow one unanalyzed successor (greedy).
            for (BasicBlock *succ : successors(cur)) {
                auto sMeta = solution.blockMeta.find(succ);
                if (sMeta == solution.blockMeta.end() || !sMeta->second.analyzed) {
                    toVisit.push_back(succ);
                    break;
                }
            }
        }

        if (synPath.empty())
            continue;

        // Find an analyzed successor as the end boundary.
        BasicBlock *endBound = nullptr;
        BasicBlock *lastBB = synPath.back();
        for (BasicBlock *succ : successors(lastBB)) {
            auto sMeta = solution.blockMeta.find(succ);
            if (sMeta != solution.blockMeta.end() && sMeta->second.analyzed) {
                endBound = succ;
                break;
            }
        }

        // Run RCG on the synthetic path.
        RCGSolver solver(synPath, state, *ctx.cfg, params, solution.blockMeta,
                         solution.decidedPlacements, startBound, endBound, &vmTracker);
        RCGResult result = solver.solve();

        if (!result.feasible) {
            PLOGE << "SCHEMATIC infeasible: energy capacity too small for function '" << F.getName()
                  << "', uncovered block '" << BB->getName() << "': " << result.errorMessage;
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

        updateSolutionFromIntervals(result);
    }

    // Fix 2: Propagate energy AFTER Step 9b before Step 10.
    propagateEnergy();

    // Step 10: Single pass — resolve remaining potential edges (reference: schematic.py:468-502).
    for (const auto &[src, dst] : ctx.cfg->getEdges()) {
        auto *srcBB = const_cast<BasicBlock *>(src);
        auto *dstBB = const_cast<BasicBlock *>(dst);
        CFGEdge edge{srcBB, dstBB};
        if (solution.enabledCheckpoints.count(edge))
            continue;

        // Skip loop back-edges (handled by LoopAnalyzer).
        if (Loop *L = LI.getLoopFor(dstBB)) {
            if (dstBB == L->getHeader() && L->contains(srcBB))
                continue;
        }

        auto srcMeta = solution.blockMeta.find(srcBB);
        auto dstMeta = solution.blockMeta.find(dstBB);
        if (srcMeta == solution.blockMeta.end() || !srcMeta->second.analyzed)
            continue;
        if (dstMeta == solution.blockMeta.end() || !dstMeta->second.analyzed)
            continue;

        // Check if allocations differ (union-based: missing key = NVM).
        auto srcAlloc = solution.decidedPlacements.find(srcBB);
        auto dstAlloc = solution.decidedPlacements.find(dstBB);
        bool allocsDiffer = false;
        {
            std::map<llvm::Value *, Placement> srcMap, dstMap;
            if (srcAlloc != solution.decidedPlacements.end())
                srcMap = srcAlloc->second;
            if (dstAlloc != solution.decidedPlacements.end())
                dstMap = dstAlloc->second;
            std::set<llvm::Value *> allKeys;
            for (const auto &[k, _] : srcMap)
                allKeys.insert(k);
            for (const auto &[k, _] : dstMap)
                allKeys.insert(k);
            for (llvm::Value *v : allKeys) {
                Placement pS = srcMap.count(v) ? srcMap[v] : Placement::NVM;
                Placement pD = dstMap.count(v) ? dstMap[v] : Placement::NVM;
                if (pS != pD) {
                    allocsDiffer = true;
                    break;
                }
            }
        }

        if (allocsDiffer || srcMeta->second.E_left <= dstMeta->second.E_to_leave) {
            // Insufficient energy or incompatible allocations → enable checkpoint.
            solution.enabledCheckpoints.insert(edge);
        } else {
            // Sufficient energy, compatible allocations → propagate energy through.
            propagateEnergy();
        }
    }

    // Step 11: Collect statistics.
    for (const auto &region : solution.regions) {
        for (const auto &[gv, place] : region.allocation.placement) {
            if (place == Placement::VM)
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
        common.regionBoundaries = solution.regions.size();
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

    if (!StatsJsonOpt.empty()) {
        CommonStats c;
        c.passName = "SCHEMATIC";
        c.functionName = F.getName().str();
        c.basicBlocks = ctx.cfg->getBlocks().size();
        c.edges = ctx.cfg->getEdges().size();
        c.candidateGlobals = state.getCandidates().size();
        c.regions = solution.regions.size();
        c.regionBoundaries = solution.regions.size();
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
