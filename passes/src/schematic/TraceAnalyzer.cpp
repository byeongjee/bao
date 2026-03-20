#include "schematic/TraceAnalyzer.h"
#include "schematic/RCGSolver.h"

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

} // namespace checkpoint
