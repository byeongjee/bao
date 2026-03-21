#include "schematic/TraceAnalyzer.h"
#include "schematic/EnergyPropagation.h"
#include "schematic/RCGSolver.h"

#include <deque>
#include <set>

namespace checkpoint {

std::vector<std::vector<SchematicBlock *>>
extractNotFixedBBPaths(const std::vector<SchematicBlock *> &trace,
                       const SchematicSolution &solution) {
    // Extract the subpaths of contiguous basic blocks not fixed following a trace.
    // Reference: schematic.py:315-349 (extract_not_fixed_bb_paths).
    std::vector<std::vector<SchematicBlock *>> paths;
    std::vector<SchematicBlock *> currentPath;
    bool registeringPath = false;
    SchematicBlock *lastFixedBB = nullptr;

    for (unsigned i = 0; i < trace.size(); ++i) {
        SchematicBlock *block = trace[i];
        auto metaIt = solution.blockMeta.find(block);
        bool isFixed = metaIt != solution.blockMeta.end() && metaIt->second.analyzed;

        if (registeringPath) {
            currentPath.push_back(block);
            if (isFixed) {
                paths.push_back(std::move(currentPath));
                currentPath.clear();
                registeringPath = false;
            }
        }
        if (!registeringPath) {
            if (isFixed) {
                lastFixedBB = block;
            } else {
                registeringPath = true;
                if (lastFixedBB) {
                    currentPath.push_back(lastFixedBB);
                    lastFixedBB = nullptr;
                }
                currentPath.push_back(block);
            }
        }
    }
    if (registeringPath) {
        paths.push_back(std::move(currentPath));
    }

    return paths;
}

bool analyzeTrace(const std::vector<SchematicBlock *> &trace, SchematicSolution &solution,
                  const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                  const SchematicParams &params, VMAddressTracker *tracker, llvm::LoopInfo &LI,
                  llvm::Loop *loopScope, std::string &errorMessage) {
    // Skip if all blocks already analyzed.
    bool allAnalyzed = true;
    for (SchematicBlock *block : trace) {
        auto it = solution.blockMeta.find(block);
        if (it == solution.blockMeta.end() || !it->second.analyzed) {
            allAnalyzed = false;
            break;
        }
    }
    if (allAnalyzed)
        return true;

    // Reference: schematic.py:351-379 (analyse_trace).
    auto subTraces = extractNotFixedBBPaths(trace, solution);

    for (const auto &subPath : subTraces) {
        RCGSolver solver(subPath, state, cfg, params, solution.blockMeta, solution.blockAllocation,
                         tracker);
        RCGResult result = solver.solve();

        if (!result.feasible) {
            errorMessage = result.errorMessage;
            return false;
        }

        applyMemoryAllocation(result, subPath, solution, cfg, state, params, LI, loopScope);
    }

    return true;
}

std::vector<SchematicBlock *> extractNotFixedBBTrace(SchematicBlock *startBB,
                                                     const SchematicSolution &solution) {
    // Reference: schematic.py:295-313 (extract_not_fixed_bb_trace).
    // trace = [predecessor_fixed_bb, unfixed_1, ..., unfixed_n, successor_fixed_bb]
    std::vector<SchematicBlock *> trace;

    // Prepend fixed predecessor (ref: trace = [next(cfg.predecessors(start_bb))]).
    for (SchematicBlock *pred : startBB->predecessors()) {
        auto pMeta = solution.blockMeta.find(pred);
        if (pMeta != solution.blockMeta.end() && pMeta->second.analyzed) {
            trace.push_back(pred);
            break;
        }
    }

    // Walk forward through unanalyzed blocks.
    std::deque<SchematicBlock *> toVisit;
    toVisit.push_back(startBB);
    std::set<SchematicBlock *> visited;
    SchematicBlock *lastBB = nullptr;
    while (!toVisit.empty()) {
        SchematicBlock *bb = toVisit.back();
        toVisit.pop_back();
        if (visited.count(bb))
            continue;
        auto curMeta = solution.blockMeta.find(bb);
        if (curMeta != solution.blockMeta.end() && curMeta->second.analyzed)
            continue;
        visited.insert(bb);
        trace.push_back(bb);
        lastBB = bb;
        for (SchematicBlock *neighbor : bb->successors()) {
            auto sMeta = solution.blockMeta.find(neighbor);
            if (sMeta == solution.blockMeta.end() || !sMeta->second.analyzed) {
                toVisit.push_back(neighbor);
                break;
            }
        }
    }

    // Append fixed successor (ref: trace.append(next(cfg.neighbors(bb)))).
    if (lastBB) {
        for (SchematicBlock *succ : lastBB->successors()) {
            auto sMeta = solution.blockMeta.find(succ);
            if (sMeta != solution.blockMeta.end() && sMeta->second.analyzed) {
                trace.push_back(succ);
                break;
            }
        }
    }

    return trace;
}

bool findAndAnalyzeNotFixedPaths(const CFGAnalysis &cfg, SchematicSolution &solution,
                                 const SchematicStateAnalysis &state, const SchematicParams &params,
                                 VMAddressTracker *tracker, llvm::LoopInfo &LI,
                                 llvm::Loop *loopScope, SchematicGraph &graph,
                                 std::string &errorMessage) {
    // Reference: schematic.py:504-518 (find_and_analyse_not_fixed_paths).
    // When loopScope is set, iterate only loop blocks (Python passes loop_cfg
    // whose .nodes contains only the loop's blocks).
    for (const llvm::BasicBlock *constBB : cfg.getBlocks()) {
        auto *BB = const_cast<llvm::BasicBlock *>(constBB);
        if (loopScope && !loopScope->contains(BB))
            continue;
        SchematicBlock *block = graph.getOrCreate(BB);
        auto metaIt = solution.blockMeta.find(block);
        if (metaIt != solution.blockMeta.end() && metaIt->second.analyzed)
            continue;

        auto trace = extractNotFixedBBTrace(block, solution);
        if (!analyzeTrace(trace, solution, state, cfg, params, tracker, LI, loopScope,
                          errorMessage))
            return false;
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
            // Same llvm::Value* => same name and type, so compare placement + VM offset.
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
                                               SchematicGraph &graph, llvm::Loop *loopScope) {
    for (const auto &[src, dst] : cfg.getEdges()) {
        auto *srcBB = const_cast<llvm::BasicBlock *>(src);
        auto *dstBB = const_cast<llvm::BasicBlock *>(dst);

        // When scoped to a loop, skip edges outside it.
        if (loopScope && (!loopScope->contains(srcBB) || !loopScope->contains(dstBB)))
            continue;

        SchematicBlock *srcBlock = graph.getOrCreate(srcBB);
        SchematicBlock *dstBlock = graph.getOrCreate(dstBB);

        CFGEdge edge{srcBlock, dstBlock};
        if (solution.enabledCheckpoints.count(edge))
            continue;

        // Skip loop back-edges (handled by LoopAnalyzer).
        if (llvm::Loop *L = LI.getLoopFor(dstBB)) {
            if (dstBB == L->getHeader() && L->contains(srcBB))
                continue;
        }

        auto srcMeta = solution.blockMeta.find(srcBlock);
        auto dstMeta = solution.blockMeta.find(dstBlock);
        if (srcMeta == solution.blockMeta.end() || !srcMeta->second.analyzed)
            continue;
        if (dstMeta == solution.blockMeta.end() || !dstMeta->second.analyzed)
            continue;

        // Reference: schematic.py:483-491 — both blocks must have memory_allocation.
        auto srcAllocIt = solution.blockAllocation.find(srcBlock);
        auto dstAllocIt = solution.blockAllocation.find(dstBlock);
        if (srcAllocIt == solution.blockAllocation.end() || !srcAllocIt->second ||
            dstAllocIt == solution.blockAllocation.end() || !dstAllocIt->second)
            continue;

        // Reference: schematic.py:494 — is_compatible_with (memory_allocation.py:162-166).
        if (!isCompatibleWith(*srcAllocIt->second, *dstAllocIt->second, state)) {
            // Not compatible -> enable checkpoint (ACTIVE).
            // Reference: schematic.py:495.
            solution.enabledCheckpoints.insert(edge);
        } else {
            // Reference: schematic.py:497 — energy_left > energy_to_leave.
            if (srcMeta->second.E_left > dstMeta->second.E_to_leave) {
                // Compatible and enough energy -> disabled, propagate.
                // Reference: schematic.py:498-500.
                propagateEnergyLeft(edge, srcMeta->second.E_left, solution, cfg, state, params, LI,
                                    loopScope);
                propagateEnergyToLeave(edge, dstMeta->second.E_to_leave, solution, cfg, state,
                                       params, LI, loopScope);
            } else {
                // Compatible but insufficient energy -> enable checkpoint (ACTIVE).
                // Reference: schematic.py:502.
                solution.enabledCheckpoints.insert(edge);
            }
        }
    }
}

} // namespace checkpoint
