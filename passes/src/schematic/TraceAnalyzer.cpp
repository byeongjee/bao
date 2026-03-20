#include "schematic/TraceAnalyzer.h"
#include "schematic/RCGSolver.h"

#include "llvm/IR/CFG.h"

#include <deque>
#include <set>

namespace checkpoint {

ExtractedSegments extractNotFixedBBPaths(const std::vector<llvm::BasicBlock *> &trace,
                                         const SchematicSolution &solution) {
    ExtractedSegments result;
    std::vector<llvm::BasicBlock *> currentSeg;

    for (unsigned i = 0; i < trace.size(); ++i) {
        llvm::BasicBlock *BB = trace[i];
        auto metaIt = solution.blockMeta.find(BB);
        bool isAnalyzed = metaIt != solution.blockMeta.end() && metaIt->second.analyzed;

        if (!isAnalyzed) {
            if (currentSeg.empty()) {
                // Record start boundary (previous analyzed block).
                llvm::BasicBlock *startBound = nullptr;
                if (i > 0) {
                    auto prevMeta = solution.blockMeta.find(trace[i - 1]);
                    if (prevMeta != solution.blockMeta.end() && prevMeta->second.analyzed)
                        startBound = trace[i - 1];
                }
                result.startBoundaries.push_back(startBound);
            }
            currentSeg.push_back(BB);
        } else {
            if (!currentSeg.empty()) {
                result.endBoundaries.push_back(BB);
                result.segments.push_back(std::move(currentSeg));
                currentSeg.clear();
            }
        }
    }
    if (!currentSeg.empty()) {
        result.endBoundaries.push_back(nullptr);
        result.segments.push_back(std::move(currentSeg));
    }

    return result;
}

bool analyzeTrace(const std::vector<llvm::BasicBlock *> &trace, SchematicSolution &solution,
                  const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                  const SchematicParams &params, VMAddressTracker *tracker, llvm::LoopInfo &LI,
                  llvm::Loop *loopScope, std::string &errorMessage) {
    // Skip if all blocks already analyzed.
    bool allAnalyzed = true;
    for (llvm::BasicBlock *BB : trace) {
        auto it = solution.blockMeta.find(BB);
        if (it == solution.blockMeta.end() || !it->second.analyzed) {
            allAnalyzed = false;
            break;
        }
    }
    if (allAnalyzed)
        return true;

    auto extracted = extractNotFixedBBPaths(trace, solution);

    for (unsigned s = 0; s < extracted.segments.size(); ++s) {
        llvm::BasicBlock *startBound =
            s < extracted.startBoundaries.size() ? extracted.startBoundaries[s] : nullptr;
        llvm::BasicBlock *endBound =
            s < extracted.endBoundaries.size() ? extracted.endBoundaries[s] : nullptr;

        RCGSolver solver(extracted.segments[s], state, cfg, params, solution.blockMeta,
                         solution.decidedPlacements, startBound, endBound, tracker);
        RCGResult result = solver.solve();

        if (!result.feasible) {
            errorMessage = result.errorMessage;
            return false;
        }

        applyMemoryAllocation(result, extracted.segments[s], startBound, endBound, solution, cfg,
                              state, params, LI, loopScope);
    }

    return true;
}

ExtractedTrace extractNotFixedBBTrace(llvm::BasicBlock *startBB,
                                      const SchematicSolution &solution) {
    ExtractedTrace result;
    result.startBoundary = nullptr;
    result.endBoundary = nullptr;

    // Find an analyzed predecessor as the start boundary.
    for (llvm::BasicBlock *pred : llvm::predecessors(startBB)) {
        auto pMeta = solution.blockMeta.find(pred);
        if (pMeta != solution.blockMeta.end() && pMeta->second.analyzed) {
            result.startBoundary = pred;
            break;
        }
    }

    // Walk forward through unanalyzed blocks.
    std::set<llvm::BasicBlock *> visited;
    std::deque<llvm::BasicBlock *> toVisit;
    toVisit.push_back(startBB);
    while (!toVisit.empty()) {
        llvm::BasicBlock *cur = toVisit.back();
        toVisit.pop_back();
        if (visited.count(cur))
            continue;
        auto curMeta = solution.blockMeta.find(cur);
        if (curMeta != solution.blockMeta.end() && curMeta->second.analyzed)
            continue;
        visited.insert(cur);
        result.blocks.push_back(cur);
        // Follow one unanalyzed successor (greedy).
        for (llvm::BasicBlock *succ : llvm::successors(cur)) {
            auto sMeta = solution.blockMeta.find(succ);
            if (sMeta == solution.blockMeta.end() || !sMeta->second.analyzed) {
                toVisit.push_back(succ);
                break;
            }
        }
    }

    // Find an analyzed successor as the end boundary.
    if (!result.blocks.empty()) {
        llvm::BasicBlock *lastBB = result.blocks.back();
        for (llvm::BasicBlock *succ : llvm::successors(lastBB)) {
            auto sMeta = solution.blockMeta.find(succ);
            if (sMeta != solution.blockMeta.end() && sMeta->second.analyzed) {
                result.endBoundary = succ;
                break;
            }
        }
    }

    return result;
}

bool findAndAnalyzeNotFixedPaths(const CFGAnalysis &cfg, SchematicSolution &solution,
                                 const SchematicStateAnalysis &state, const SchematicParams &params,
                                 VMAddressTracker *tracker, llvm::LoopInfo &LI,
                                 llvm::Loop *loopScope, std::string &errorMessage) {
    for (const llvm::BasicBlock *constBB : cfg.getBlocks()) {
        auto *BB = const_cast<llvm::BasicBlock *>(constBB);
        auto metaIt = solution.blockMeta.find(BB);
        if (metaIt != solution.blockMeta.end() && metaIt->second.analyzed)
            continue;

        auto extracted = extractNotFixedBBTrace(BB, solution);
        if (extracted.blocks.empty())
            continue;

        RCGSolver solver(extracted.blocks, state, cfg, params, solution.blockMeta,
                         solution.decidedPlacements, extracted.startBoundary, extracted.endBoundary,
                         tracker);
        RCGResult result = solver.solve();

        if (!result.feasible) {
            errorMessage = result.errorMessage;
            return false;
        }

        applyMemoryAllocation(result, extracted.blocks, extracted.startBoundary,
                              extracted.endBoundary, solution, cfg, state, params, LI, loopScope);
    }

    return true;
}

} // namespace checkpoint
