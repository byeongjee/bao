#include "schematic/MemoryAllocator.h"
#include "schematic/EnergyPropagation.h"
#include "schematic/RCGSolver.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include <algorithm>
#include <vector>

namespace checkpoint {

void VMAddressTracker::reset() {
    allocatedVars_.clear();
    topAddress_ = 0;
}

std::optional<unsigned> VMAddressTracker::getExistingAddress(llvm::Value *v) const {
    auto it = allocatedVars_.find(v);
    if (it != allocatedVars_.end())
        return it->second;
    return std::nullopt;
}

unsigned VMAddressTracker::recordAllocation(llvm::Value *v, unsigned size) {
    unsigned addr = topAddress_;
    allocatedVars_[v] = addr;
    topAddress_ += size;
    return addr;
}

unsigned VMAddressTracker::getTopAddress() const {
    return topAddress_;
}

std::pair<bool, bool> computeSaveRestoreFlags(llvm::Value *v,
                                              const std::vector<SchematicBlock *> &intervalBlocks,
                                              const SchematicStateAnalysis &state) {

    // needRestore: true if the first access to v in the interval is a load.
    bool needRestore = false;
    for (SchematicBlock *block : intervalBlocks) {
        auto *BB = block->getLLVMBlock();
        if (!BB)
            continue;
        auto firstOp = state.getFirstOpIsLoad(BB, v);
        if (firstOp.has_value()) {
            needRestore = *firstOp; // true if first op is load
            break;
        }
    }

    // NOTE: A more optimal implementation would compute needSave based on
    // liveness analysis (whether the variable is live-out of the interval),
    // but the reference SCHEMATIC algorithm unconditionally assumes save is needed.
    bool needSave = true;

    return {needRestore, needSave};
}

double estimateEnergyGain(unsigned accessCount, unsigned varSizeBytes, bool needRestore,
                          bool needSave, const SchematicParams &params) {
    double E_sr = params.memRestoreEnergyPerByte * varSizeBytes * (needRestore ? 1.0 : 0.0) +
                  params.memStoreEnergyPerByte * varSizeBytes * (needSave ? 1.0 : 0.0);
    return params.nvmAccessPenalty * accessCount - E_sr;
}

std::optional<RegionAllocation>
mergeAllocations(const std::vector<const RegionAllocation *> &allocations,
                 const SchematicStateAnalysis &state, bool checkpointIncreaseAllowed) {
    // Reference: memory_allocation.py:merge_allocations (line 217).
    if (allocations.empty())
        return RegionAllocation{};

    // Start with a copy of the first allocation (reference: deepcopy(allocations[0]))
    RegionAllocation result = *allocations[0];
    if (allocations.size() == 1)
        return result;

    // Track occupied memory intervals for overlap detection.
    // Reference: MemoryAllocation.occupied_memory
    std::vector<std::pair<unsigned, unsigned>> occupied;
    for (const auto &[v, va] : result.vars) {
        if (va.placement == Placement::VM) {
            auto offIt = result.vmOffsets.find(v);
            if (offIt != result.vmOffsets.end()) {
                unsigned size = state.getVarSizeBytes(v);
                occupied.push_back({offIt->second, offIt->second + size});
            }
        }
    }

    for (unsigned i = 1; i < allocations.size(); ++i) {
        for (const auto &[v, va] : allocations[i]->vars) {
            auto existIt = result.vars.find(v);
            if (existIt != result.vars.end()) {
                // Variable already in result — check compatibility.
                // Reference: add_new_var_alloc: if var_alloc != self.vars[name]: raise
                if (existIt->second.placement != va.placement)
                    return std::nullopt;
                if (va.placement == Placement::VM) {
                    auto off1 = result.vmOffsets.find(v);
                    auto off2 = allocations[i]->vmOffsets.find(v);
                    if (off1 != result.vmOffsets.end() && off2 != allocations[i]->vmOffsets.end() &&
                        off1->second != off2->second)
                        return std::nullopt;
                }
            } else {
                // Variable not yet in result.
                // Reference: add_new_var_alloc lines 188-214.
                if (va.placement == Placement::VM) {
                    auto offIt = allocations[i]->vmOffsets.find(v);
                    if (offIt != allocations[i]->vmOffsets.end()) {
                        unsigned startAddr = offIt->second;
                        unsigned size = state.getVarSizeBytes(v);
                        unsigned endAddr = startAddr + size;

                        // Check address interval is empty.
                        // Reference: interval_is_empty (line 138).
                        bool overlaps = false;
                        for (const auto &[s, e] : occupied) {
                            if (startAddr < e && s < endAddr) {
                                overlaps = true;
                                break;
                            }
                        }
                        if (overlaps)
                            return std::nullopt;

                        // Check checkpoint increase.
                        // Reference: add_new_var_alloc line 200.
                        if ((va.needRestore() || va.needSave()) && !checkpointIncreaseAllowed)
                            return std::nullopt;

                        result.vars[v] = va;
                        result.vmOffsets[v] = offIt->second;
                        occupied.push_back({startAddr, endAddr});
                    } else {
                        if ((va.needRestore() || va.needSave()) && !checkpointIncreaseAllowed)
                            return std::nullopt;
                        result.vars[v] = va;
                    }
                } else {
                    // NVM variable — just add it.
                    result.vars[v] = va;
                }
            }
        }
    }

    return result;
}

std::pair<RegionAllocation, double>
chooseMemoryAllocation(const std::vector<SchematicBlock *> &intervalBlocks,
                       const SchematicStateAnalysis &state, const SchematicParams &params,
                       const RegionAllocation *startAlloc, const RegionAllocation *endAlloc,
                       const std::vector<const RegionAllocation *> &memoryAllocations,
                       VMAddressTracker *tracker, unsigned accessScale) {

    // Step 1: Merge allocations (reference: memory_allocator.py:153-163).
    RegionAllocation constrainedAlloc;
    {
        std::optional<RegionAllocation> merged;
        if (!memoryAllocations.empty()) {
            merged = mergeAllocations(memoryAllocations, state, /*checkpointIncreaseAllowed=*/true);
            if (!merged) {
                return {RegionAllocation{}, -99999.0};
            }
            constrainedAlloc = std::move(*merged);
        }
        if (startAlloc) {
            std::vector<const RegionAllocation *> toMerge = {startAlloc, &constrainedAlloc};
            merged = mergeAllocations(toMerge, state, /*checkpointIncreaseAllowed=*/false);
            if (!merged) {
                return {RegionAllocation{}, -99999.0};
            }
            constrainedAlloc = std::move(*merged);
        }
        if (endAlloc) {
            std::vector<const RegionAllocation *> toMerge = {endAlloc, &constrainedAlloc};
            merged = mergeAllocations(toMerge, state, /*checkpointIncreaseAllowed=*/false);
            if (!merged) {
                return {RegionAllocation{}, -99999.0};
            }
            constrainedAlloc = std::move(*merged);
        }
    }

    RegionAllocation result = constrainedAlloc;
    double totalGain = 0.0;

    // Step 2: Subtract save/restore costs for constrained variables.
    // Reference lines 167-170: use need_restore/need_save from merged allocation.
    for (const auto &[v, va] : result.vars) {
        unsigned size = state.getVarSizeBytes(v);
        if (va.needRestore())
            totalGain -= params.memRestoreEnergyPerByte * size;
        if (va.needSave())
            totalGain -= params.memStoreEnergyPerByte * size;
    }

    // Step 3: Evaluate candidates not in constrained allocation.
    // Candidate struct for greedy packing.
    struct CandidateEntry {
        llvm::Value *v;
        double gain;
        unsigned size;
        bool needRestore;
        bool needSave;
    };
    std::vector<CandidateEntry> candidates;

    for (llvm::Value *v : state.getCandidates()) {
        if (result.vars.count(v))
            continue;

        // Accumulate access counts across interval, scaled by accessScale
        // (used by convergence loop to account for multiple iterations).
        unsigned nR = 0, nW = 0;
        for (SchematicBlock *block : intervalBlocks) {
            auto *BB = block->getLLVMBlock();
            if (!BB)
                continue;
            nR += state.getLoadCount(BB, v);
            nW += state.getStoreCount(BB, v);
        }
        if (nR == 0 && nW == 0)
            continue;
        nR *= accessScale;
        nW *= accessScale;

        auto [needRestore, needSave] = computeSaveRestoreFlags(v, intervalBlocks, state);

        // Reference: memory_allocator.py:179-181 — force pointer-type variables to NVM.
        bool isPointerType = false;
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(v))
            isPointerType = GV->getValueType()->isPointerTy();
        else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(v))
            isPointerType = AI->getAllocatedType()->isPointerTy();
        if (isPointerType) {
            result.vars[v] = {Placement::NVM, needRestore, needSave};
            continue;
        }

        // Reference lines 182-190: two-branch structure matching Python exactly.
        if (!startAlloc && !endAlloc) {
            unsigned size = state.getVarSizeBytes(v);
            double gain = estimateEnergyGain(nR + nW, size, needRestore, needSave, params);
            candidates.push_back({v, gain, size, needRestore, needSave});
        } else {
            if ((needRestore && startAlloc) || (needSave && endAlloc)) {
                result.vars[v] = {Placement::NVM, needRestore, needSave};
            } else {
                unsigned size = state.getVarSizeBytes(v);
                double gain = estimateEnergyGain(nR + nW, size, needRestore, needSave, params);
                candidates.push_back({v, gain, size, needRestore, needSave});
            }
        }
    }

    // Sort candidates by gain descending.
    // Reference: Counter(energy_gains).most_common() (line 192).
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateEntry &a, const CandidateEntry &b) { return a.gain > b.gain; });

    // Greedy pack into VM.
    // Reference lines 198-226: curr_address starts at tracker's top_address.
    unsigned currAddress = tracker ? tracker->getTopAddress() : 0;
    for (const auto &c : candidates) {
        VariableAllocation varAlloc{Placement::NVM, c.needRestore, c.needSave};
        if (c.gain > 0.0) {
            // Reference lines 208-213: reuse existing VM address.
            if (tracker) {
                auto existing = tracker->getExistingAddress(c.v);
                if (existing) {
                    varAlloc.placement = Placement::VM;
                    result.vmOffsets[c.v] = *existing;
                    totalGain += c.gain;
                    result.vars[c.v] = varAlloc;
                    continue;
                }
            }

            // Reference lines 215-221: allocate new VM address.
            if (currAddress + c.size <= params.vmCapacityBytes) {
                varAlloc.placement = Placement::VM;
                result.vmOffsets[c.v] = currAddress;
                currAddress += c.size;
                if (tracker)
                    tracker->recordAllocation(c.v, c.size);
                totalGain += c.gain;
                result.vars[c.v] = varAlloc;
                continue;
            }
        }
        result.vars[c.v] = varAlloc;
    }

    // Reference lines 224-226: update vmBytesUsed.
    result.vmBytesUsed = currAddress;

    return {std::move(result), totalGain};
}

ComputeCostResult computeCost(
    const std::vector<SchematicBlock *> &blocks, const SchematicStateAnalysis &state,
    const CFGAnalysis &cfg, const SchematicParams &params,
    const std::unordered_map<SchematicBlock *, std::shared_ptr<RegionAllocation>> &blockAllocation,
    VMAddressTracker *tracker, const RegionAllocation *startAlloc,
    const RegionAllocation *endAlloc) {
    // Reference: memory_allocator.py:compute_cost lines 241-242.
    if (blocks.empty())
        return {RegionAllocation{}, 0.0};

    // Execution energy: sum of all blocks at all-NVM cost.
    // Reference: memory_allocator.py:compute_cost lines 248-250.
    double energy = 0.0;
    // Collect memory_allocations from blocks (no dedup — matches Python).
    // Reference: memory_allocator.py:compute_cost lines 247-252.
    std::vector<const RegionAllocation *> memoryAllocations;
    for (SchematicBlock *block : blocks) {
        if (auto *BB = block->getLLVMBlock())
            energy += cfg.getBlockInfo(BB).energyCost;
        auto it = blockAllocation.find(block);
        if (it != blockAllocation.end())
            memoryAllocations.push_back(it->second.get());
    }

    // Choose memory allocation and subtract gain.
    // Reference: memory_allocator.py:compute_cost lines 260-263.
    auto [alloc, gain] = chooseMemoryAllocation(blocks, state, params, startAlloc, endAlloc,
                                                memoryAllocations, tracker, 1);
    energy -= gain;

    return {std::move(alloc), energy};
}

double computeAllocationRestoreCost(
    SchematicBlock *block,
    const std::unordered_map<SchematicBlock *, std::map<llvm::Value *, Placement>>
        &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params) {
    double cost = 0.0;
    auto allocIt = decidedPlacements.find(block);
    if (allocIt != decidedPlacements.end()) {
        for (const auto &[gv, place] : allocIt->second) {
            if (place == Placement::VM)
                cost += params.memRestoreEnergyPerByte * state.getVarSizeBytes(gv);
        }
    }
    return cost;
}

double computeAllocationSaveCost(
    SchematicBlock *block,
    const std::unordered_map<SchematicBlock *, std::map<llvm::Value *, Placement>>
        &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params) {
    double cost = 0.0;
    auto allocIt = decidedPlacements.find(block);
    if (allocIt != decidedPlacements.end()) {
        for (const auto &[gv, place] : allocIt->second) {
            if (place == Placement::VM)
                cost += params.memStoreEnergyPerByte * state.getVarSizeBytes(gv);
        }
    }
    return cost;
}

/// Extend allocation `target` with variables from `source` that are not yet present.
/// Reference: memory_allocation.py:MemoryAllocation.extends (line 153).
static void extendsAllocation(RegionAllocation &target, const RegionAllocation &source) {
    for (const auto &[v, va] : source.vars) {
        if (target.vars.find(v) == target.vars.end()) {
            target.vars[v] = va;
            if (va.placement == Placement::VM) {
                auto offIt = source.vmOffsets.find(v);
                if (offIt != source.vmOffsets.end())
                    target.vmOffsets[v] = offIt->second;
            }
        }
    }
}

void updateCheckpointType(const std::vector<CFGEdge> &selectedCheckpoints,
                          SchematicSolution &solution) {
    for (const auto &ckpt : selectedCheckpoints)
        solution.enabledCheckpoints.insert(resolveCheckpointEdge(ckpt));
}

void applyMemoryAllocation(const RCGResult &result, const std::vector<SchematicBlock *> &trace,
                           SchematicSolution &solution, const CFGAnalysis &cfg,
                           const SchematicStateAnalysis &state, const SchematicParams &params,
                           llvm::LoopInfo &LI, llvm::Loop *loopScope) {
    // Reference: schematic.py:397-398.
    if (trace.size() < 3)
        llvm::report_fatal_error("Trace should be at least 3 bb long (start, bb and end)");

    // 1. Mark checkpoints as enabled
    updateCheckpointType(result.selectedCheckpoints, solution);

    // 1b. Boundary allocation extension (reference: schematic.py:402-421).
    // When no checkpoint separates a boundary from the adjacent interval,
    // extend the boundary's allocation with the interval's variables.
    // Work on a mutable copy of allocations since result is const.
    std::vector<RegionAllocation> allocations = result.allocations;

    if (!allocations.empty()) {
        // Start boundary extension.
        // Condition: trace[0] has allocation AND (no checkpoints OR first ckpt src !=
        // trace[0]).
        auto startAllocIt = solution.blockAllocation.find(trace.front());
        if (startAllocIt != solution.blockAllocation.end()) {
            bool noSeparation = result.selectedCheckpoints.empty() ||
                                result.selectedCheckpoints.front().src != trace.front();
            if (noSeparation) {
                extendsAllocation(*startAllocIt->second, allocations.front());
                // Use boundary's extended allocation as the interval allocation.
                allocations.front() = *startAllocIt->second;
            }
        }

        // End boundary extension.
        // Condition: trace[-1] has allocation AND (no checkpoints OR last ckpt dst != trace[-1]).
        auto endAllocIt = solution.blockAllocation.find(trace.back());
        if (endAllocIt != solution.blockAllocation.end()) {
            bool noSeparation = result.selectedCheckpoints.empty() ||
                                result.selectedCheckpoints.back().src != trace.back();
            if (noSeparation) {
                extendsAllocation(*endAllocIt->second, allocations.back());
                allocations.back() = *endAllocIt->second;
            }
        }

        // No checkpoints: unify start and end boundary allocations (reference: lines 414-421).
        if (result.selectedCheckpoints.empty()) {
            auto sIt = solution.blockAllocation.find(trace.front());
            auto eIt = solution.blockAllocation.find(trace.back());
            if (sIt != solution.blockAllocation.end())
                extendsAllocation(*sIt->second, allocations.front());
            if (eIt != solution.blockAllocation.end())
                extendsAllocation(*eIt->second, allocations.front());
            // Replace start boundary's allocation object with end boundary's
            // so all blocks sharing the old object get unified.
            if (sIt != solution.blockAllocation.end() && eIt != solution.blockAllocation.end()) {
                auto oldAlloc = sIt->second;
                auto newAlloc = eIt->second;
                if (oldAlloc != newAlloc) {
                    for (auto &[bb, allocPtr] : solution.blockAllocation) {
                        if (allocPtr == oldAlloc)
                            allocPtr = newAlloc;
                    }
                }
            }
        }
    }

    // 2. Record allocations and mark blocks as analyzed.
    // Reference: schematic.py:427-447 — walk entire trace applying allocations.
    unsigned i = 0;
    std::shared_ptr<RegionAllocation> memoryAlloc;
    for (unsigned j = 0; j < allocations.size(); ++j) {
        memoryAlloc = std::make_shared<RegionAllocation>(allocations[j]);
        bool checkpointReached = false;
        while (i < trace.size() - 1 && !checkpointReached) {
            auto &meta = solution.blockMeta[trace[i]];
            meta.analyzed = true;
            for (const auto &[gv, va] : allocations[j].vars)
                solution.decidedPlacements[trace[i]][gv] = va.placement;
            solution.blockAllocation[trace[i]] = memoryAlloc;

            // Reference: schematic.py:442 — check if edge matches next checkpoint.
            if (j < result.selectedCheckpoints.size() &&
                trace[i] == result.selectedCheckpoints[j].src &&
                trace[i + 1] == result.selectedCheckpoints[j].dst) {
                checkpointReached = true;
            }
            ++i;
        }
        solution.regions.push_back({result.intervalBlocks[j], allocations[j]});
    }
    // Handle the last basic block (reference: schematic.py:447).
    if (memoryAlloc) {
        auto &meta = solution.blockMeta[trace[i]];
        meta.analyzed = true;
        for (const auto &[gv, va] : memoryAlloc->vars)
            solution.decidedPlacements[trace[i]][gv] = va.placement;
        solution.blockAllocation[trace[i]] = memoryAlloc;
    }

    // 3. Per-checkpoint energy propagation (reference: apply_memory_allocation lines 449-466)
    struct SeedCkpt {
        SchematicBlock *bbBefore;
        SchematicBlock *bbAfter;
        bool isVirtual;
    };
    std::vector<SeedCkpt> ckpts;
    ckpts.push_back({nullptr, trace.front(), /*isVirtual=*/true});
    for (const auto &ckptEdge : result.selectedCheckpoints)
        ckpts.push_back({ckptEdge.src, ckptEdge.dst, /*isVirtual=*/false});
    ckpts.push_back({trace.back(), nullptr, /*isVirtual=*/true});

    for (const auto &checkpoint : ckpts) {
        if (checkpoint.bbAfter) {
            double energyLeftStart;
            auto metaIt = solution.blockMeta.find(checkpoint.bbAfter);
            // Reference line 453: virtual checkpoint with existing value
            if (checkpoint.isVirtual && metaIt != solution.blockMeta.end() &&
                metaIt->second.E_left < std::numeric_limits<double>::max()) {
                energyLeftStart =
                    metaIt->second.E_left +
                    getBlockExecEnergy(checkpoint.bbAfter, solution, cfg, state, params);
            } else {
                energyLeftStart =
                    params.capacity - params.E_pro - params.N_reg * params.regRestoreEnergy -
                    computeAllocationRestoreCost(checkpoint.bbAfter, solution.decidedPlacements,
                                                 state, params);
            }
            CFGEdge fwdEdge{checkpoint.bbBefore, checkpoint.bbAfter};
            // Synthetic blocks now have proper successors via SchematicGraph,
            // so energy propagation via SchematicBlock's successors() works directly.
            propagateEnergyLeft(fwdEdge, energyLeftStart, solution, cfg, state, params, LI,
                                loopScope);
        }

        if (checkpoint.bbBefore) {
            double energyToLeave;
            auto metaIt = solution.blockMeta.find(checkpoint.bbBefore);
            // Reference line 461: existing nonzero value -> undo block cost
            if (metaIt != solution.blockMeta.end() && metaIt->second.E_to_leave != 0.0) {
                energyToLeave =
                    metaIt->second.E_to_leave -
                    getBlockExecEnergy(checkpoint.bbBefore, solution, cfg, state, params);
            } else {
                energyToLeave = params.E_epi + params.N_reg * params.regStoreEnergy +
                                computeAllocationSaveCost(
                                    checkpoint.bbBefore, solution.decidedPlacements, state, params);
            }
            CFGEdge bwdEdge{checkpoint.bbBefore, checkpoint.bbAfter};
            // Synthetic blocks now have proper predecessors via SchematicGraph,
            // so energy propagation via SchematicBlock's predecessors() works directly.
            propagateEnergyToLeave(bwdEdge, energyToLeave, solution, cfg, state, params, LI,
                                   loopScope);
        }
    }
}

} // namespace checkpoint
