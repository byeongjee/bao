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
                                              const std::vector<llvm::BasicBlock *> &intervalBlocks,
                                              const SchematicStateAnalysis &state) {

    // needRestore: true if the first access to v in the interval is a load.
    bool needRestore = false;
    for (llvm::BasicBlock *BB : intervalBlocks) {
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

RegionAllocation
chooseMemoryAllocation(const std::vector<llvm::BasicBlock *> &intervalBlocks,
                       const SchematicStateAnalysis &state, const SchematicParams &params,
                       const std::map<llvm::Value *, Placement> &fixedPlacements,
                       VMAddressTracker *tracker, const RegionAllocation *startConstraint,
                       const RegionAllocation *endConstraint, unsigned accessScale) {

    RegionAllocation result;

    // Copy fixed placements and compute initial VM usage.
    // Also compute livenessFlags for fixed placements — without these, the
    // instrumenter skips save/restore memcpy, which can cause infinite loops
    // when shadow variables go out of sync with their backing allocas.
    result.placement = fixedPlacements;
    result.vmBytesUsed = 0;
    for (const auto &[v, place] : fixedPlacements) {
        auto [needRestore, needSave] = computeSaveRestoreFlags(v, intervalBlocks, state);
        result.livenessFlags[v] = {needRestore, needSave};
        if (place == Placement::VM) {
            unsigned size = state.getVarSizeBytes(v);
            if (tracker) {
                auto existing = tracker->getExistingAddress(v);
                result.vmOffsets[v] = existing ? *existing : tracker->recordAllocation(v, size);
            } else {
                result.vmOffsets[v] = result.vmBytesUsed;
            }
            result.vmBytesUsed += size;
        }
    }

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
        if (fixedPlacements.count(v))
            continue;

        // Accumulate access counts across interval, scaled by accessScale
        // (used by convergence loop to account for multiple iterations).
        unsigned nR = 0, nW = 0;
        for (llvm::BasicBlock *BB : intervalBlocks) {
            nR += state.getLoadCount(BB, v);
            nW += state.getStoreCount(BB, v);
        }
        if (nR == 0 && nW == 0)
            continue;
        nR *= accessScale;
        nW *= accessScale;

        auto [needRestore, needSave] = computeSaveRestoreFlags(v, intervalBlocks, state);

        // Reference lines 186-190: if startConstraint exists and variable
        // needs restore, or endConstraint exists and variable needs save,
        // force to NVM (skip as candidate).
        bool forcedNvm = false;
        if (startConstraint && needRestore) {
            forcedNvm = true;
        }
        if (endConstraint && needSave) {
            forcedNvm = true;
        }
        if (forcedNvm) {
            result.placement[v] = Placement::NVM;
            result.livenessFlags[v] = {needRestore, needSave};
            continue;
        }

        // Reference: memory_allocator.py:179-181 — force pointer-type variables to NVM.
        bool isPointerType = false;
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(v))
            isPointerType = GV->getValueType()->isPointerTy();
        else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(v))
            isPointerType = AI->getAllocatedType()->isPointerTy();
        if (isPointerType) {
            result.placement[v] = Placement::NVM;
            result.livenessFlags[v] = {needRestore, needSave};
            continue;
        }

        unsigned size = state.getVarSizeBytes(v);
        double gain = estimateEnergyGain(nR + nW, size, needRestore, needSave, params);

        candidates.push_back({v, gain, size, needRestore, needSave});
    }

    // Sort positive-gain candidates by raw gain descending.
    // Tie-break: prefer smaller size (packs more variables into VM).
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateEntry &a, const CandidateEntry &b) {
                  if (a.gain != b.gain)
                      return a.gain > b.gain;
                  return a.size < b.size;
              });

    // Greedy pack into VM.
    for (const auto &c : candidates) {
        if (c.gain > 0.0) {
            // If tracker knows this variable, reuse its address (reference
            // lines 206-213: previously allocated variables reuse their VM address,
            // but only when gain > 0).
            if (tracker) {
                auto existing = tracker->getExistingAddress(c.v);
                if (existing) {
                    result.placement[c.v] = Placement::VM;
                    result.vmOffsets[c.v] = *existing;
                    result.vmBytesUsed += c.size;
                    result.livenessFlags[c.v] = {c.needRestore, c.needSave};
                    continue;
                }
            }

            if (result.vmBytesUsed + c.size <= params.vmCapacityBytes) {
                result.placement[c.v] = Placement::VM;
                if (tracker) {
                    result.vmOffsets[c.v] = tracker->recordAllocation(c.v, c.size);
                } else {
                    result.vmOffsets[c.v] = result.vmBytesUsed;
                }
                result.vmBytesUsed += c.size;
                result.livenessFlags[c.v] = {c.needRestore, c.needSave};
                continue;
            }
        }

        result.placement[c.v] = Placement::NVM;
        result.livenessFlags[c.v] = {c.needRestore, c.needSave};
    }

    return result;
}

ComputeCostResult computeCost(const std::vector<llvm::BasicBlock *> &blocks,
                              const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                              const SchematicParams &params,
                              const std::map<llvm::Value *, Placement> &fixedPlacements,
                              VMAddressTracker *tracker, const RegionAllocation *startConstraint,
                              const RegionAllocation *endConstraint) {
    // Execution energy: sum of all blocks at all-NVM cost
    double energy = 0.0;
    for (llvm::BasicBlock *BB : blocks)
        energy += cfg.getBlockInfo(BB).energyCost;

    // Choose memory allocation and subtract gain
    auto alloc = chooseMemoryAllocation(blocks, state, params, fixedPlacements, tracker,
                                        startConstraint, endConstraint, 1);
    double gain = computeMemoryAllocationGain(alloc, blocks, state, params);
    energy -= gain;

    return {std::move(alloc), energy};
}

double computeMemoryAllocationGain(const RegionAllocation &alloc,
                                   const std::vector<llvm::BasicBlock *> &blocks,
                                   const SchematicStateAnalysis &state,
                                   const SchematicParams &params) {
    double gain = 0.0;
    for (const auto &[v, place] : alloc.placement) {
        if (place != Placement::VM)
            continue;
        unsigned accesses = 0;
        for (llvm::BasicBlock *BB : blocks)
            accesses += state.getLoadCount(BB, v) + state.getStoreCount(BB, v);
        gain += params.nvmAccessPenalty * accesses;
    }
    return gain;
}

double computeAllocationRestoreCost(
    llvm::BasicBlock *BB,
    const llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>> &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params) {
    double cost = 0.0;
    auto allocIt = decidedPlacements.find(BB);
    if (allocIt != decidedPlacements.end()) {
        for (const auto &[gv, place] : allocIt->second) {
            if (place == Placement::VM)
                cost += params.memRestoreEnergyPerByte * state.getVarSizeBytes(gv);
        }
    }
    return cost;
}

double computeAllocationSaveCost(
    llvm::BasicBlock *BB,
    const llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>> &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params) {
    double cost = 0.0;
    auto allocIt = decidedPlacements.find(BB);
    if (allocIt != decidedPlacements.end()) {
        for (const auto &[gv, place] : allocIt->second) {
            if (place == Placement::VM)
                cost += params.memStoreEnergyPerByte * state.getVarSizeBytes(gv);
        }
    }
    return cost;
}

void updateCheckpointType(const std::vector<CFGEdge> &selectedCheckpoints,
                          SchematicSolution &solution) {
    for (const auto &ckpt : selectedCheckpoints)
        solution.enabledCheckpoints.insert(resolveCheckpointEdge(ckpt));
}

void applyMemoryAllocation(const RCGResult &result,
                           const std::vector<llvm::BasicBlock *> &pathBlocks,
                           llvm::BasicBlock *startBound, llvm::BasicBlock *endBound,
                           SchematicSolution &solution, const CFGAnalysis &cfg,
                           const SchematicStateAnalysis &state, const SchematicParams &params,
                           llvm::LoopInfo &LI, llvm::Loop *loopScope) {
    // 1. Mark checkpoints as enabled
    updateCheckpointType(result.selectedCheckpoints, solution);

    // 2. Record allocations and mark blocks as analyzed
    for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
        const auto &blocks = result.intervalBlocks[i];
        const auto &alloc = result.allocations[i];

        for (llvm::BasicBlock *BB : blocks) {
            auto &meta = solution.blockMeta[BB];
            meta.analyzed = true;
            for (const auto &[gv, place] : alloc.placement)
                solution.decidedPlacements[BB][gv] = place;
        }

        solution.regions.push_back({blocks, alloc});
    }

    // 3. Per-checkpoint energy propagation (reference: apply_memory_allocation lines 449-466)
    struct SeedCkpt {
        llvm::BasicBlock *bbBefore;
        llvm::BasicBlock *bbAfter;
        bool isVirtual;
    };
    std::vector<SeedCkpt> ckpts;
    ckpts.push_back({startBound, pathBlocks.front(), /*isVirtual=*/true});
    for (const auto &ckptEdge : result.selectedCheckpoints)
        ckpts.push_back({ckptEdge.src, ckptEdge.dst, /*isVirtual=*/false});
    ckpts.push_back({pathBlocks.back(), endBound, /*isVirtual=*/true});

    for (const auto &ck : ckpts) {
        if (ck.bbAfter) {
            double energyLeftStart;
            auto metaIt = solution.blockMeta.find(ck.bbAfter);
            // Reference line 453: virtual checkpoint with existing value
            if (ck.isVirtual && metaIt != solution.blockMeta.end() &&
                metaIt->second.E_left < std::numeric_limits<double>::max()) {
                energyLeftStart = metaIt->second.E_left +
                                  getBlockExecEnergy(ck.bbAfter, solution, cfg, state, params);
            } else {
                energyLeftStart =
                    params.capacity - params.E_pro - params.N_reg * params.regRestoreEnergy -
                    computeAllocationRestoreCost(ck.bbAfter, solution.decidedPlacements, state,
                                                 params);
            }
            CFGEdge fwdEdge{ck.bbBefore, ck.bbAfter};
            propagateEnergyLeft(fwdEdge, energyLeftStart, solution, cfg, state, params, LI,
                                loopScope);
        }

        if (ck.bbBefore) {
            double eToLeave;
            auto metaIt = solution.blockMeta.find(ck.bbBefore);
            // Reference line 461: existing nonzero value → undo block cost
            if (metaIt != solution.blockMeta.end() && metaIt->second.E_to_leave != 0.0) {
                eToLeave = metaIt->second.E_to_leave -
                           getBlockExecEnergy(ck.bbBefore, solution, cfg, state, params);
            } else {
                eToLeave = params.E_epi + params.N_reg * params.regStoreEnergy +
                           computeAllocationSaveCost(ck.bbBefore, solution.decidedPlacements, state,
                                                     params);
            }
            CFGEdge bwdEdge{ck.bbBefore, ck.bbAfter};
            propagateEnergyToLeave(bwdEdge, eToLeave, solution, cfg, state, params, LI, loopScope);
        }
    }
}

} // namespace checkpoint
