#include "schematic/IntervalAllocator.h"

#include <algorithm>
#include <vector>

namespace checkpoint {

std::pair<bool, bool> computeLivenessFlags(
    llvm::GlobalVariable *v,
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const StateAnalysis &state,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {

    // live_start: true if the first access to v in the interval is a load.
    bool liveStart = false;
    for (llvm::BasicBlock *BB : intervalBlocks) {
        unsigned loads = state.getLoadCount(BB, v);
        unsigned stores = state.getStoreCount(BB, v);
        if (loads > 0) {
            liveStart = true;
            break;
        }
        if (stores > 0) {
            liveStart = false;
            break;
        }
    }

    // live_end: if postIntervalBlocks provided, true if v is accessed in any
    // post-interval block. Otherwise conservatively true (assume live-out).
    bool liveEnd;
    if (postIntervalBlocks) {
        liveEnd = false;
        for (llvm::BasicBlock *BB : *postIntervalBlocks) {
            if (state.getLoadCount(BB, v) > 0 ||
                state.getStoreCount(BB, v) > 0) {
                liveEnd = true;
                break;
            }
        }
    } else {
        liveEnd = true;
    }

    return {liveStart, liveEnd};
}

RegionAllocation computeIntervalAllocation(
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const StateAnalysis &state,
    const SchematicParams &params,
    const std::map<llvm::GlobalVariable *, Placement> &fixedPlacements,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {

    RegionAllocation result;

    // Copy fixed placements and compute initial VM usage.
    result.placement = fixedPlacements;
    result.vmBytesUsed = 0;
    for (const auto &[gv, place] : fixedPlacements) {
        if (place == Placement::VM) {
            unsigned size = state.getVarSizeBytes(gv);
            result.vmOffsets[gv] = result.vmBytesUsed;
            result.vmBytesUsed += size;
        }
    }

    // Candidate struct for greedy packing.
    struct Candidate {
        llvm::GlobalVariable *gv;
        double gain;
        unsigned size;
        bool liveStart;
        bool liveEnd;
    };
    std::vector<Candidate> candidates;

    for (llvm::GlobalVariable *v : state.getVMObjs()) {
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

        auto [liveStart, liveEnd] =
            computeLivenessFlags(v, intervalBlocks, state, postIntervalBlocks);

        unsigned size = state.getVarSizeBytes(v);
        double E_sr = params.memRestoreEnergyPerByte * size * (liveStart ? 1.0 : 0.0) +
                      params.memStoreEnergyPerByte * size * (liveEnd ? 1.0 : 0.0);
        double gain = params.nvmAccessPenalty * (nR + nW) - E_sr;

        candidates.push_back({v, gain, size, liveStart, liveEnd});
    }

    // Sort positive-gain candidates by gain/size descending (density-based greedy).
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  double densA = a.size > 0 ? a.gain / a.size : 0.0;
                  double densB = b.size > 0 ? b.gain / b.size : 0.0;
                  return densA > densB;
              });

    // Greedy pack into VM.
    for (const auto &c : candidates) {
        if (c.gain > 0.0 &&
            result.vmBytesUsed + c.size <= params.vmCapacityBytes) {
            result.placement[c.gv] = Placement::VM;
            result.vmOffsets[c.gv] = result.vmBytesUsed;
            result.vmBytesUsed += c.size;
            result.livenessFlags[c.gv] = {c.liveStart, c.liveEnd};
        } else {
            result.placement[c.gv] = Placement::NVM;
            result.livenessFlags[c.gv] = {c.liveStart, c.liveEnd};
        }
    }

    return result;
}

double computeIntervalEnergy(
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const RegionAllocation &allocation,
    const StateAnalysis &state,
    const CFGAnalysis &cfg,
    const SchematicParams &params,
    bool isFirstInterval,
    bool isLastInterval,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {

    // E_restore: checkpoint restore cost at interval start.
    double E_restore = 0.0;
    if (!isFirstInterval) {
        E_restore = params.E_pro + params.N_reg * params.regRestoreEnergy;
        for (const auto &[gv, place] : allocation.placement) {
            if (place != Placement::VM)
                continue;
            auto flagIt = allocation.livenessFlags.find(gv);
            if (flagIt != allocation.livenessFlags.end() && flagIt->second.first) {
                E_restore +=
                    params.memRestoreEnergyPerByte * state.getVarSizeBytes(gv);
            }
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
            auto flagIt = allocation.livenessFlags.find(gv);
            if (flagIt != allocation.livenessFlags.end() && flagIt->second.second) {
                E_save +=
                    params.memStoreEnergyPerByte * state.getVarSizeBytes(gv);
            }
        }
    }

    return E_restore + E_exec + E_save;
}

} // namespace checkpoint
