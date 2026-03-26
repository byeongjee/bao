#include "schematic/TraceAnalyzer.h"
#include "schematic/EnergyPropagation.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/RCGSolver.h"

#include <deque>
#include <limits>
#include <optional>
#include <set>

namespace checkpoint {

static bool isCompatibleWith(const RegionAllocation &a, const RegionAllocation &b,
                             const SchematicStateAnalysis &state);

namespace {

struct FixedPotentialCheckpointEdge {
    llvm::BasicBlock *srcBB;
    llvm::BasicBlock *dstBB;
    SchematicBlock *srcBlock;
    SchematicBlock *dstBlock;
    CFGEdge edge;
    BlockMetadata *srcMeta;
    BlockMetadata *dstMeta;
    RegionAllocation *srcAlloc;
    RegionAllocation *dstAlloc;
};

static std::string getBlockName(const SchematicBlock *block) {
    if (const llvm::BasicBlock *BB = block->getLLVMBlock())
        return BB->getName().str();
    return block->getName().str();
}

static std::string describeFixedEdgeOrigin(llvm::Loop *loopScope, llvm::StringRef reason,
                                           llvm::BasicBlock *srcBB, llvm::BasicBlock *dstBB) {
    std::string scope = loopScope ? "loop" : "function";
    return scope + "-" + reason.str() + "[" + srcBB->getName().str() + " -> " +
           dstBB->getName().str() + "]";
}

static bool isEdgeOutsideLoopScope(llvm::Loop *loopScope, llvm::BasicBlock *srcBB,
                                   llvm::BasicBlock *dstBB) {
    return loopScope && (!loopScope->contains(srcBB) || !loopScope->contains(dstBB));
}

static bool isLoopBackEdge(llvm::LoopInfo &LI, llvm::BasicBlock *srcBB, llvm::BasicBlock *dstBB) {
    if (llvm::Loop *L = LI.getLoopFor(dstBB))
        return dstBB == L->getHeader() && L->contains(srcBB);
    return false;
}

static BlockMetadata *getAnalyzedBlockMeta(SchematicSolution &solution, SchematicBlock *block) {
    auto it = solution.blockMeta.find(block);
    if (it == solution.blockMeta.end() || !it->second.analyzed)
        return nullptr;
    return &it->second;
}

static RegionAllocation *getResolvedAllocation(SchematicSolution &solution, SchematicBlock *block) {
    auto it = solution.blockAllocation.find(block);
    if (it == solution.blockAllocation.end() || !it->second)
        return nullptr;
    return it->second.get();
}

static std::optional<FixedPotentialCheckpointEdge> getFixedPotentialCheckpointEdge(
    const std::pair<const llvm::BasicBlock *, const llvm::BasicBlock *> &cfgEdge,
    SchematicSolution &solution, llvm::LoopInfo &LI, SchematicGraph &graph, llvm::Loop *loopScope) {
    auto *srcBB = const_cast<llvm::BasicBlock *>(cfgEdge.first);
    auto *dstBB = const_cast<llvm::BasicBlock *>(cfgEdge.second);
    if (isEdgeOutsideLoopScope(loopScope, srcBB, dstBB))
        return std::nullopt;

    SchematicBlock *srcBlock = graph.getOrCreate(srcBB);
    SchematicBlock *dstBlock = graph.getOrCreate(dstBB);
    CFGEdge edge{srcBlock, dstBlock};
    if (!isPotentialCheckpoint(solution, edge))
        return std::nullopt;
    if (isLoopBackEdge(LI, srcBB, dstBB))
        return std::nullopt;

    BlockMetadata *srcMeta = getAnalyzedBlockMeta(solution, srcBlock);
    BlockMetadata *dstMeta = getAnalyzedBlockMeta(solution, dstBlock);
    if (!srcMeta || !dstMeta)
        return std::nullopt;

    RegionAllocation *srcAlloc = getResolvedAllocation(solution, srcBlock);
    RegionAllocation *dstAlloc = getResolvedAllocation(solution, dstBlock);
    if (!srcAlloc || !dstAlloc)
        return std::nullopt;

    return FixedPotentialCheckpointEdge{srcBB,   dstBB,   srcBlock, dstBlock, edge,
                                        srcMeta, dstMeta, srcAlloc, dstAlloc};
}

static bool canMergeWithoutCheckpoint(const FixedPotentialCheckpointEdge &edge) {
    return edge.srcMeta->E_left > edge.dstMeta->E_to_leave;
}

static void enableFixedEdgeCheckpoint(SchematicSolution &solution,
                                      const FixedPotentialCheckpointEdge &edge,
                                      llvm::Loop *loopScope, llvm::StringRef reason) {
    enableCheckpoint(solution, edge.edge,
                     describeFixedEdgeOrigin(loopScope, reason, edge.srcBB, edge.dstBB));
}

static void disableFixedEdgeCheckpointAndPropagate(const FixedPotentialCheckpointEdge &edge,
                                                   const CFGAnalysis &cfg,
                                                   SchematicSolution &solution,
                                                   const SchematicStateAnalysis &state,
                                                   const SchematicParams &params,
                                                   llvm::LoopInfo &LI, llvm::Loop *loopScope) {
    disableCheckpoint(solution, edge.edge);
    propagateEnergyLeft(edge.edge, edge.srcMeta->E_left, solution, cfg, state, params, LI,
                        loopScope);
    propagateEnergyToLeave(edge.edge, edge.dstMeta->E_to_leave, solution, cfg, state, params, LI,
                           loopScope);
}

} // namespace

static std::string describeTraceOrigin(const std::vector<SchematicBlock *> &trace,
                                       llvm::Loop *loopScope) {
    std::string scope = loopScope ? "loop" : "function";
    std::string start = getBlockName(trace.front());
    std::string end = getBlockName(trace.back());
    return scope + "-rcg[" + start + " -> " + end + "]";
}

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

static bool allocationsAreCompatible(const FixedPotentialCheckpointEdge &edge,
                                     const SchematicStateAnalysis &state) {
    return isCompatibleWith(*edge.srcAlloc, *edge.dstAlloc, state);
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

    for (auto &subPath : subTraces) {
        // When forceCheckpointOnIncompatibleLoops is enabled, check if the
        // start and end block allocations are incompatible.  If so, force a
        // checkpoint at the boundary and truncate the sub-trace so the RCG
        // does not attempt to merge the incompatible end allocation.
        // This mirrors the paper's lines 2-3 (back-edge checkpoint on
        // allocation mismatch) but applies it to inner-loop boundaries.
        if (params.forceCheckpointOnIncompatibleLoops && subPath.size() >= 3) {
            auto *startBlock = subPath.front();
            auto *endBlock = subPath.back();
            auto sIt = solution.blockAllocation.find(startBlock);
            auto eIt = solution.blockAllocation.find(endBlock);
            if (sIt != solution.blockAllocation.end() && sIt->second &&
                eIt != solution.blockAllocation.end() && eIt->second &&
                (!mergeAllocations({sIt->second.get(), eIt->second.get()}, state,
                                   /*checkpointIncreaseAllowed=*/false) ||
                 !mergeAllocations({eIt->second.get(), sIt->second.get()}, state,
                                   /*checkpointIncreaseAllowed=*/false))) {
                // Force checkpoint at the edge entering the end block.
                CFGEdge forced{subPath[subPath.size() - 2], endBlock};
                std::string origin =
                    (loopScope ? "loop" : "function") + std::string("-forced-incompatible[") +
                    (startBlock->getLLVMBlock() ? startBlock->getLLVMBlock()->getName().str()
                                                : startBlock->getName().str()) +
                    " -> " +
                    (endBlock->getLLVMBlock() ? endBlock->getLLVMBlock()->getName().str()
                                              : endBlock->getName().str()) +
                    "]";
                enableCheckpoint(solution, forced, origin);
                // Reset E_left for all blocks in the current loop scope
                // so that later sub-traces use a fresh energy budget
                // (the forced checkpoint creates a new region boundary,
                // invalidating energy accounting for downstream blocks).
                for (auto &[block, meta] : solution.blockMeta) {
                    if (llvm::BasicBlock *bb = block->getLLVMBlock()) {
                        if (!loopScope || loopScope->contains(bb))
                            meta.E_left = std::numeric_limits<double>::max();
                    }
                }
                llvm::errs() << "[SCHEMATIC] Forced checkpoint at "
                             << (forced.src->getLLVMBlock() ? forced.src->getLLVMBlock()->getName()
                                                            : forced.src->getName())
                             << " -> "
                             << (forced.dst->getLLVMBlock() ? forced.dst->getLLVMBlock()->getName()
                                                            : forced.dst->getName())
                             << " due to incompatible loop allocations\n";
                subPath.pop_back();

                if (subPath.size() < 3) {
                    // Only one unfixed block remains — assign start block's
                    // allocation directly (no RCG needed).
                    auto startAlloc = sIt->second;
                    for (unsigned i = 1; i < subPath.size(); ++i) {
                        solution.blockMeta[subPath[i]].analyzed = true;
                        solution.blockAllocation[subPath[i]] = startAlloc;
                        for (const auto &[gv, va] : startAlloc->vars)
                            solution.decidedPlacements[subPath[i]][gv] = va.placement;
                    }
                    continue;
                }
            }
        }

        RCGSolver solver(subPath, state, cfg, params, solution.blockMeta, solution.blockAllocation,
                         tracker);
        RCGResult result = solver.solve();

        if (!result.feasible) {
            errorMessage = result.errorMessage;
            return false;
        }

        applyMemoryAllocation(result, subPath, solution, cfg, state, params, LI, loopScope,
                              describeTraceOrigin(subPath, loopScope));
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

void removePotentialCheckpointsBetweenFixedBBs(const CFGAnalysis &cfg, SchematicSolution &solution,
                                               const SchematicStateAnalysis &state,
                                               const SchematicParams &params, llvm::LoopInfo &LI,
                                               SchematicGraph &graph, llvm::Loop *loopScope) {
    for (const auto &cfgEdge : cfg.getEdges()) {
        auto edge = getFixedPotentialCheckpointEdge(cfgEdge, solution, LI, graph, loopScope);
        if (!edge)
            continue;

        if (!allocationsAreCompatible(*edge, state)) {
            enableFixedEdgeCheckpoint(solution, *edge, loopScope, "fixed-edge-incompatible");
            continue;
        }

        if (canMergeWithoutCheckpoint(*edge)) {
            disableFixedEdgeCheckpointAndPropagate(*edge, cfg, solution, state, params, LI,
                                                   loopScope);
            continue;
        }

        enableFixedEdgeCheckpoint(solution, *edge, loopScope, "fixed-edge-energy");
    }
}

} // namespace checkpoint
