#include "schematic/TraceAnalyzer.h"
#include "schematic/EnergyPropagation.h"
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
                         solution.blockAllocation, startBound, endBound, tracker);
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
                         solution.blockAllocation, extracted.startBoundary, extracted.endBoundary,
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

/// Reference: memory_allocation.py:140-149 (interval_is_empty).
/// Returns true if the given [startAddr, endAddr) interval does not overlap
/// any VM-placed variable's address range in this allocation.
static bool intervalIsEmpty(const RegionAllocation &alloc, unsigned startAddr, unsigned endAddr,
                            const SchematicStateAnalysis &state) {
    for (const auto &[v, va] : alloc.vars) {
        if (va.placement != Placement::VM)
            continue;
        auto offIt = alloc.vmOffsets.find(v);
        if (offIt == alloc.vmOffsets.end())
            continue;
        unsigned iStart = offIt->second;
        unsigned iEnd = iStart + state.getVarSizeBytes(v);
        if (startAddr < iEnd && iStart < endAddr)
            return false;
    }
    return true;
}

/// Reference: memory_allocation.py:151-160 (_is_compatible_with).
/// One-way compatibility: for each var in other, if not in self and VM-placed,
/// check that its address range is free in self. If in both, check equality.
static bool isCompatibleOneWay(const RegionAllocation &self, const RegionAllocation &other,
                               const SchematicStateAnalysis &state) {
    for (const auto &[v, varAlloc] : other.vars) {
        auto selfIt = self.vars.find(v);
        if (selfIt == self.vars.end()) {
            // Reference: memory_allocation.py:154-156.
            if (varAlloc.placement == Placement::VM) {
                auto offIt = other.vmOffsets.find(v);
                if (offIt == other.vmOffsets.end())
                    continue;
                unsigned startAddr = offIt->second;
                unsigned endAddr = startAddr + state.getVarSizeBytes(v);
                if (!intervalIsEmpty(self, startAddr, endAddr, state))
                    return false;
            }
        } else {
            // Reference: memory_allocation.py:158-159 (var_alloc != self.vars[name]).
            // Python __eq__ compares: allocation, len(type), name, start_address, end_address.
            // Same llvm::Value* ⇒ same name and type, so compare placement + VM offset.
            if (varAlloc.placement != selfIt->second.placement)
                return false;
            if (varAlloc.placement == Placement::VM) {
                auto selfOff = self.vmOffsets.find(v);
                auto otherOff = other.vmOffsets.find(v);
                unsigned selfAddr = (selfOff != self.vmOffsets.end()) ? selfOff->second : 0;
                unsigned otherAddr = (otherOff != other.vmOffsets.end()) ? otherOff->second : 0;
                if (selfAddr != otherAddr)
                    return false;
            }
        }
    }
    return true;
}

/// Reference: memory_allocation.py:162-166 (is_compatible_with).
/// Bidirectional compatibility check.
static bool isCompatibleWith(const RegionAllocation &a, const RegionAllocation &b,
                             const SchematicStateAnalysis &state) {
    return isCompatibleOneWay(a, b, state) && isCompatibleOneWay(b, a, state);
}

void removePotentialCheckpointsBetweenFixedBBs(const CFGAnalysis &cfg, SchematicSolution &solution,
                                               const SchematicStateAnalysis &state,
                                               const SchematicParams &params, llvm::LoopInfo &LI,
                                               llvm::Loop *loopScope) {
    for (const auto &[src, dst] : cfg.getEdges()) {
        auto *srcBB = const_cast<llvm::BasicBlock *>(src);
        auto *dstBB = const_cast<llvm::BasicBlock *>(dst);

        // When scoped to a loop, skip edges outside it.
        if (loopScope && (!loopScope->contains(srcBB) || !loopScope->contains(dstBB)))
            continue;

        CFGEdge edge{srcBB, dstBB};
        if (solution.enabledCheckpoints.count(edge))
            continue;

        // Skip loop back-edges (handled by LoopAnalyzer).
        if (llvm::Loop *L = LI.getLoopFor(dstBB)) {
            if (dstBB == L->getHeader() && L->contains(srcBB))
                continue;
        }

        auto srcMeta = solution.blockMeta.find(srcBB);
        auto dstMeta = solution.blockMeta.find(dstBB);
        if (srcMeta == solution.blockMeta.end() || !srcMeta->second.analyzed)
            continue;
        if (dstMeta == solution.blockMeta.end() || !dstMeta->second.analyzed)
            continue;

        // Reference: schematic.py:483-491 — both blocks must have memory_allocation.
        auto srcAllocIt = solution.blockAllocation.find(srcBB);
        auto dstAllocIt = solution.blockAllocation.find(dstBB);
        if (srcAllocIt == solution.blockAllocation.end() || !srcAllocIt->second ||
            dstAllocIt == solution.blockAllocation.end() || !dstAllocIt->second)
            continue;

        // Reference: schematic.py:494 — is_compatible_with (memory_allocation.py:162-166).
        if (!isCompatibleWith(*srcAllocIt->second, *dstAllocIt->second, state)) {
            // Not compatible → enable checkpoint (ACTIVE).
            // Reference: schematic.py:495.
            solution.enabledCheckpoints.insert(edge);
        } else {
            // Reference: schematic.py:497 — energy_left > energy_to_leave.
            if (srcMeta->second.E_left > dstMeta->second.E_to_leave) {
                // Compatible and enough energy → disabled, propagate.
                // Reference: schematic.py:498-500.
                propagateEnergyLeft(edge, srcMeta->second.E_left, solution, cfg, state, params, LI,
                                    loopScope);
                propagateEnergyToLeave(edge, dstMeta->second.E_to_leave, solution, cfg, state,
                                       params, LI, loopScope);
            } else {
                // Compatible but insufficient energy → enable checkpoint (ACTIVE).
                // Reference: schematic.py:502.
                solution.enabledCheckpoints.insert(edge);
            }
        }
    }
}

} // namespace checkpoint
