#include "schematic/SchematicPass.h"

#include "milp/CheckpointContext.h"
#include "milp/StateAnalysis.h"
#include "schematic/LoopAnalyzer.h"
#include "schematic/RCGSolver.h"
#include "schematic/SchematicInstrumenter.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/TraceLoader.h"
#include "common/BlockUtils.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"

#include <chrono>
#include <deque>
#include <set>
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> SchematicConfigOpt;
extern cl::opt<std::string> SchematicTraceOpt;
extern cl::opt<bool> AddDebugMarkersOpt;

namespace checkpoint {

PreservedAnalyses SchematicPass::run(Function &F,
                                     FunctionAnalysisManager &AM) {
    const auto totalStart = std::chrono::steady_clock::now();

    // Step 1: Obtain LLVM analyses.
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);

    // Step 2: Create base checkpoint context (estimator + CFG).
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(),
                                             "schematic pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip())
            errs() << ctxResult.errorMessage;
        return PreservedAnalyses::all();
    }
    auto &ctx = *ctxResult.context;

    // Step 3: Parse SCHEMATIC params.
    auto paramsOpt = parseSchematicParams(SchematicConfigOpt.getValue());
    if (!paramsOpt) {
        errs() << "Error: Failed to parse SCHEMATIC config: "
               << SchematicConfigOpt.getValue() << "\n";
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
        for (auto it = BB.begin(); it != BB.end(); ) {
            auto *AI = dyn_cast<AllocaInst>(&*it++);
            if (AI && isa<ConstantInt>(AI->getArraySize()))
                AI->moveBefore(entryBB, entryBB.getFirstInsertionPt());
        }
    }

    // Step 5: Run StateAnalysis.
    StateAnalysis state(F, AA, *ctx.cfg);
    if (state.hasAnalysisErrors()) {
        state.printAnalysisErrors(errs());
        errs() << "Skipping SCHEMATIC instrumentation for function "
               << F.getName() << " due to unresolved memory/call effects.\n";
        return PreservedAnalyses::all();
    }

    SchematicSolution solution;

    // Step 6: Load traces (optional).
    std::optional<LoadedTraces> loadedTraces;
    if (!SchematicTraceOpt.getValue().empty()) {
        TraceLoader loader(F, LI);
        loadedTraces = loader.load(SchematicTraceOpt.getValue());
        if (loadedTraces)
            errs() << "SCHEMATIC: loaded traces for " << F.getName() << "\n";
    }

    // Step 7: Loop analysis.
    LoopAnalyzer loopAnalyzer(LI, SE, *ctx.cfg, state, params);
    if (loadedTraces)
        loopAnalyzer.setLoadedLoopTraces(loadedTraces->loopTraces);

    if (!loopAnalyzer.analyzeLoops(solution)) {
        errs() << "SCHEMATIC: loop analysis failed for " << F.getName()
               << " — aborting\n";
        return PreservedAnalyses::all();
    }

    // Step 8: Get paths from traces (required).
    if (!loadedTraces || loadedTraces->functionPaths.empty()) {
        errs() << "SCHEMATIC: no traces loaded for " << F.getName()
               << " — traces are required\n";
        return PreservedAnalyses::all();
    }
    std::vector<EnumeratedPath> paths = loadedTraces->functionPaths;

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
            bool isAnalyzed = metaIt != solution.blockMeta.end() &&
                              metaIt->second.analyzed;

            if (!isAnalyzed) {
                if (currentSeg.empty()) {
                    // Record start boundary (previous analyzed block).
                    llvm::BasicBlock *startBound = nullptr;
                    if (i > 0) {
                        auto prevMeta = solution.blockMeta.find(ep.blocks[i - 1]);
                        if (prevMeta != solution.blockMeta.end() &&
                            prevMeta->second.analyzed)
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
            llvm::BasicBlock *endBound =
                s < endBoundaries.size() ? endBoundaries[s] : nullptr;

            RCGSolver solver(segments[s], state, *ctx.cfg, params,
                             solution.blockMeta, solution.decidedPlacements,
                             startBound, endBound);
            RCGResult result = solver.solve();

            if (!result.feasible) {
                report_fatal_error(
                    Twine("SCHEMATIC infeasible segment in function '") +
                        F.getName() + "', path #" +
                        Twine(solution.pathsAnalyzed) + ": " +
                        result.errorMessage,
                    /*GenCrashDiag=*/false);
            }

            // Update solution from RCG result.
            for (const auto &ckpt : result.selectedCheckpoints)
                solution.enabledCheckpoints.insert(ckpt);

            for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
                const auto &blocks = result.intervalBlocks[i];
                const auto &alloc = result.allocations[i];

                // Compute energy consumed up to each block for E_left/E_to_leave.
                double energyAccum = 0.0;
                for (unsigned b = 0; b < blocks.size(); ++b) {
                    llvm::BasicBlock *BB = blocks[b];
                    double bbEnergy = ctx.cfg->getBlockInfo(BB).energyCost;
                    // Subtract NVM savings for VM-placed vars.
                    for (const auto &[gv, place] : alloc.placement) {
                        if (place != Placement::VM)
                            continue;
                        unsigned loads = state.getLoadCount(BB, gv);
                        unsigned stores = state.getStoreCount(BB, gv);
                        bbEnergy -= params.nvmAccessPenalty * (loads + stores);
                    }
                    energyAccum += bbEnergy;

                    auto &meta = solution.blockMeta[BB];
                    // Monotonic updates.
                    double newELeft = alloc.intervalEnergy - energyAccum;
                    if (!meta.analyzed || newELeft < meta.E_left)
                        meta.E_left = newELeft;
                    if (!meta.analyzed || energyAccum > meta.E_to_leave)
                        meta.E_to_leave = energyAccum;
                    meta.analyzed = true;

                    // Update decided placements.
                    for (const auto &[gv, place] : alloc.placement)
                        solution.decidedPlacements[BB][gv] = place;
                }

                // Store as region.
                solution.regions.push_back({blocks, alloc});
            }
        }
    }

    // Step 9b: Analyze uncovered blocks (Python: find_and_analyse_not_fixed_paths).
    // For each block not yet analyzed, build a synthetic path:
    //   [analyzed predecessor] → unfixed blocks (greedy walk) → [analyzed successor]
    // and run the same RCG analysis on it.
    for (const BasicBlock *constBB : ctx.cfg->getBlocks()) {
        BasicBlock *BB = const_cast<BasicBlock *>(constBB);
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
        RCGSolver solver(synPath, state, *ctx.cfg, params,
                         solution.blockMeta, solution.decidedPlacements,
                         startBound, endBound);
        RCGResult result = solver.solve();

        if (!result.feasible) {
            report_fatal_error(
                Twine("SCHEMATIC infeasible synthetic path in function '") +
                    F.getName() + "', uncovered block '" + BB->getName() +
                    "': " + result.errorMessage,
                /*GenCrashDiag=*/false);
        }

        // Update solution (same logic as Step 9).
        for (const auto &ckpt : result.selectedCheckpoints)
            solution.enabledCheckpoints.insert(ckpt);

        for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
            const auto &blocks = result.intervalBlocks[i];
            const auto &alloc = result.allocations[i];

            double energyAccum = 0.0;
            for (unsigned b = 0; b < blocks.size(); ++b) {
                BasicBlock *blk = blocks[b];
                double bbEnergy = ctx.cfg->getBlockInfo(blk).energyCost;
                for (const auto &[gv, place] : alloc.placement) {
                    if (place != Placement::VM)
                        continue;
                    unsigned loads = state.getLoadCount(blk, gv);
                    unsigned stores = state.getStoreCount(blk, gv);
                    bbEnergy -= params.nvmAccessPenalty * (loads + stores);
                }
                energyAccum += bbEnergy;

                auto &meta = solution.blockMeta[blk];
                double newELeft = alloc.intervalEnergy - energyAccum;
                if (!meta.analyzed || newELeft < meta.E_left)
                    meta.E_left = newELeft;
                if (!meta.analyzed || energyAccum > meta.E_to_leave)
                    meta.E_to_leave = energyAccum;
                meta.analyzed = true;

                for (const auto &[gv, place] : alloc.placement)
                    solution.decidedPlacements[blk][gv] = place;
            }

            solution.regions.push_back({blocks, alloc});
        }
    }

    // Step 10: Resolve remaining potential edges between analyzed blocks.
    for (const auto &[src, dst] : ctx.cfg->getEdges()) {
        CFGEdge edge{const_cast<BasicBlock *>(src),
                     const_cast<BasicBlock *>(dst)};
        if (solution.enabledCheckpoints.count(edge))
            continue;

        auto srcMeta = solution.blockMeta.find(const_cast<BasicBlock *>(src));
        auto dstMeta = solution.blockMeta.find(const_cast<BasicBlock *>(dst));
        if (srcMeta == solution.blockMeta.end() || !srcMeta->second.analyzed)
            continue;
        if (dstMeta == solution.blockMeta.end() || !dstMeta->second.analyzed)
            continue;

        // Check if allocations differ.
        auto srcAlloc = solution.decidedPlacements.find(
            const_cast<BasicBlock *>(src));
        auto dstAlloc = solution.decidedPlacements.find(
            const_cast<BasicBlock *>(dst));
        bool allocsDiffer = false;
        if (srcAlloc != solution.decidedPlacements.end() &&
            dstAlloc != solution.decidedPlacements.end()) {
            for (const auto &[gv, place] : srcAlloc->second) {
                auto it = dstAlloc->second.find(gv);
                if (it != dstAlloc->second.end() && it->second != place) {
                    allocsDiffer = true;
                    break;
                }
            }
        }

        if (allocsDiffer) {
            solution.enabledCheckpoints.insert(edge);
        } else if (srcMeta->second.E_left < dstMeta->second.E_to_leave) {
            // Not enough energy to reach dst's checkpoint — need boundary.
            solution.enabledCheckpoints.insert(edge);
        }
        // else: E_left >= E_to_leave → no checkpoint needed.
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
    SchematicInstrumenter instrumenter(*F.getParent(), params.addDebugMarkers,
                                       params.N_reg);
    unsigned inserted = instrumenter.instrumentFunction(F, solution, state);

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Step 13: Print statistics.
    errs() << "=== SCHEMATIC Checkpoint Insertion Statistics ===\n";
    errs() << "  Function:                        " << F.getName() << "\n";
    errs() << "  Basic blocks:                    " << ctx.cfg->getBlocks().size()
           << "\n";
    errs() << "  Edges:                           " << ctx.cfg->getEdges().size()
           << "\n";
    errs() << "  Paths analyzed:                  " << solution.pathsAnalyzed
           << "\n";
    errs() << "  Candidate globals (V_elig):      "
           << state.getVMObjs().size() << "\n";
    errs() << "  Enabled checkpoints:             "
           << solution.enabledCheckpoints.size() << "\n";
    errs() << "  Loop decisions:                  "
           << solution.loopDecisions.size() << "\n";
    errs() << "  Regions:                         " << solution.regions.size()
           << "\n";
    errs() << "  Runtime calls inserted:          " << inserted << "\n";
    errs() << "  Compilation time (ms):           "
           << llvm::format("%.3f", totalExecutionTimeMs) << "\n";
    errs() << "  Peak RSS (KB):                   "
           << getPeakRSSKb() << "\n";
    errs() << "  Trace-guided:                    yes\n";

    return PreservedAnalyses::none();
}

} // namespace checkpoint
