#include "schematic/IntervalAllocator.h"

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
        unsigned loads = state.getLoadCount(BB, v);
        unsigned stores = state.getStoreCount(BB, v);
        if (loads > 0) {
            needRestore = true;
            break;
        }
        if (stores > 0) {
            needRestore = false;
            break;
        }
    }

    // NOTE: A more optimal implementation would compute needSave based on
    // liveness analysis (whether the variable is live-out of the interval),
    // but the reference SCHEMATIC algorithm unconditionally assumes save is needed.
    bool needSave = true;

    return {needRestore, needSave};
}

RegionAllocation
computeIntervalAllocation(const std::vector<llvm::BasicBlock *> &intervalBlocks,
                          const SchematicStateAnalysis &state, const SchematicParams &params,
                          const std::map<llvm::Value *, Placement> &fixedPlacements,
                          VMAddressTracker *tracker, const RegionAllocation *startConstraint,
                          const RegionAllocation *endConstraint) {

    RegionAllocation result;

    // Copy fixed placements and compute initial VM usage.
    result.placement = fixedPlacements;
    result.vmBytesUsed = 0;
    for (const auto &[v, place] : fixedPlacements) {
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

        // Accumulate access counts across interval.
        unsigned nR = 0, nW = 0;
        for (llvm::BasicBlock *BB : intervalBlocks) {
            nR += state.getLoadCount(BB, v);
            nW += state.getStoreCount(BB, v);
        }
        if (nR == 0 && nW == 0)
            continue;

        auto [needRestore, needSave] = computeSaveRestoreFlags(v, intervalBlocks, state);

        // Reference lines 186-190: if startConstraint exists and variable
        // needs restore, or endConstraint exists and variable needs save,
        // force to NVM (skip as candidate).
        bool forcedNvm = false;
        if (startConstraint && needRestore && startConstraint->placement.count(v)) {
            forcedNvm = true;
        }
        if (endConstraint && needSave && endConstraint->placement.count(v)) {
            forcedNvm = true;
        }
        if (forcedNvm) {
            result.placement[v] = Placement::NVM;
            result.livenessFlags[v] = {needRestore, needSave};
            continue;
        }

        unsigned size = state.getVarSizeBytes(v);
        double E_sr = params.memRestoreEnergyPerByte * size * (needRestore ? 1.0 : 0.0) +
                      params.memStoreEnergyPerByte * size * (needSave ? 1.0 : 0.0);
        double gain = params.nvmAccessPenalty * (nR + nW) - E_sr;

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
        // If tracker knows this variable, reuse its address (matching reference
        // lines 208-213: previously allocated variables are always placed in VM).
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

        if (c.gain > 0.0 && result.vmBytesUsed + c.size <= params.vmCapacityBytes) {
            result.placement[c.v] = Placement::VM;
            if (tracker) {
                result.vmOffsets[c.v] = tracker->recordAllocation(c.v, c.size);
            } else {
                result.vmOffsets[c.v] = result.vmBytesUsed;
            }
            result.vmBytesUsed += c.size;
            result.livenessFlags[c.v] = {c.needRestore, c.needSave};
        } else {
            result.placement[c.v] = Placement::NVM;
            result.livenessFlags[c.v] = {c.needRestore, c.needSave};
        }
    }

    return result;
}

double computeIntervalEnergy(const std::vector<llvm::BasicBlock *> &intervalBlocks,
                             const RegionAllocation &allocation,
                             const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                             const SchematicParams &params, bool isFirstInterval,
                             bool isLastInterval) {

    // E_restore: checkpoint restore cost at interval start.
    // NOTE: A more optimal implementation would only include restore costs for
    // variables where needRestore is true (first access is a load), but the
    // reference SCHEMATIC algorithm unconditionally restores all VM variables
    // at interval boundaries.
    double E_restore = 0.0;
    if (!isFirstInterval) {
        E_restore = params.E_pro + params.N_reg * params.regRestoreEnergy;
        for (const auto &[gv, place] : allocation.placement) {
            if (place != Placement::VM)
                continue;
            E_restore += params.memRestoreEnergyPerByte * state.getVarSizeBytes(gv);
        }
    }

    // E_exec: execution energy minus NVM savings for VM-placed vars.
    double E_exec = 0.0;
    for (llvm::BasicBlock *BB : intervalBlocks) {
        E_exec += cfg.getBlockInfo(BB).energyCost;
    }
    for (const auto &[gv, place] : allocation.placement) {
        if (place != Placement::VM)
            continue;
        for (llvm::BasicBlock *BB : intervalBlocks) {
            unsigned loads = state.getLoadCount(BB, gv);
            unsigned stores = state.getStoreCount(BB, gv);
            E_exec -= params.nvmAccessPenalty * (loads + stores);
        }
    }

    // E_save: checkpoint save cost at interval end.
    double E_save = 0.0;
    if (!isLastInterval) {
        E_save = params.E_epi + params.N_reg * params.regStoreEnergy;
        for (const auto &[gv, place] : allocation.placement) {
            if (place != Placement::VM)
                continue;
            E_save += params.memStoreEnergyPerByte * state.getVarSizeBytes(gv);
        }
    }

    return E_restore + E_exec + E_save;
}

} // namespace checkpoint
