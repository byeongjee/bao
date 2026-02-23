#include "schematic/IntervalAllocator.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <set>

namespace checkpoint {

namespace {

enum class AccessKind { None, Read, Write, Unknown };

static bool pointsToGlobal(const llvm::Value *Ptr, llvm::GlobalVariable *GV) {
    if (!Ptr || !GV)
        return false;
    const llvm::Value *Obj =
        llvm::getUnderlyingObject(Ptr->stripPointerCasts());
    return Obj == GV;
}

static AccessKind firstAccessInBlock(llvm::GlobalVariable *GV,
                                     llvm::BasicBlock *BB) {
    for (llvm::Instruction &I : *BB) {
        if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            if (pointsToGlobal(LI->getPointerOperand(), GV))
                return AccessKind::Read;
            continue;
        }
        if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            if (pointsToGlobal(SI->getPointerOperand(), GV))
                return AccessKind::Write;
            continue;
        }

        // Conservative fallback for memory-touching calls that reference GV.
        if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (!CB->mayReadOrWriteMemory())
                continue;
            for (const llvm::Use &U : CB->operands()) {
                const llvm::Value *V =
                    llvm::getUnderlyingObject(U.get()->stripPointerCasts());
                if (V == GV)
                    return AccessKind::Unknown;
            }
        }
    }
    return AccessKind::None;
}

static AccessKind firstAccessInBlocks(
    llvm::GlobalVariable *GV,
    const std::vector<llvm::BasicBlock *> &blocks) {

    for (llvm::BasicBlock *BB : blocks) {
        AccessKind kind = firstAccessInBlock(GV, BB);
        if (kind != AccessKind::None)
            return kind;
    }
    return AccessKind::None;
}

static bool hasAnyAccess(llvm::GlobalVariable *GV,
                         const std::vector<llvm::BasicBlock *> &blocks,
                         const StateAnalysis &state) {
    for (llvm::BasicBlock *BB : blocks) {
        if (state.getLoadCount(BB, GV) > 0 || state.getStoreCount(BB, GV) > 0)
            return true;
    }
    return false;
}

} // namespace

std::pair<bool, bool> computeLivenessFlags(
    llvm::GlobalVariable *v,
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const StateAnalysis &state,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {

    bool liveStart = false;
    bool liveEnd = false;

    // live_start: based on first access after interval start.
    switch (firstAccessInBlocks(v, intervalBlocks)) {
    case AccessKind::Read:
        liveStart = true;
        break;
    case AccessKind::Unknown:
        // Conservative fallback on unresolved access ordering.
        liveStart = true;
        break;
    case AccessKind::Write:
    case AccessKind::None:
        liveStart = false;
        break;
    }

    // live_end: if post-interval path context is available, use first access
    // after the boundary. Otherwise, use conservative fallback.
    if (postIntervalBlocks) {
        switch (firstAccessInBlocks(v, *postIntervalBlocks)) {
        case AccessKind::Read:
        case AccessKind::Unknown:
            liveEnd = true;
            break;
        case AccessKind::Write:
        case AccessKind::None:
            liveEnd = false;
            break;
        }
    } else {
        liveEnd = hasAnyAccess(v, intervalBlocks, state);
    }

    return {liveStart, liveEnd};
}

RegionAllocation computeIntervalAllocation(
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const StateAnalysis &state,
    const SchematicParams &params,
    const std::map<llvm::GlobalVariable *, Placement> &fixedPlacements,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {

    RegionAllocation alloc;
    alloc.vmBytesUsed = 0;

    // Copy fixed placements and count their VM usage
    unsigned fixedVmBytes = 0;
    for (const auto &[gv, p] : fixedPlacements) {
        alloc.placement[gv] = p;
        if (p == Placement::VM)
            fixedVmBytes += state.getVarSizeBytes(gv);
    }

    unsigned remainingVm = (fixedVmBytes >= params.vmCapacityBytes)
                               ? 0
                               : params.vmCapacityBytes - fixedVmBytes;

    // Collect candidate variables referenced in the interval
    std::set<llvm::GlobalVariable *> referenced;
    for (llvm::BasicBlock *BB : intervalBlocks) {
        for (llvm::GlobalVariable *gv : state.getVMObjs()) {
            if (state.getLoadCount(BB, gv) > 0 ||
                state.getStoreCount(BB, gv) > 0) {
                referenced.insert(gv);
            }
        }
    }

    // Compute gain for each non-fixed candidate
    struct Candidate {
        llvm::GlobalVariable *gv;
        double gain;
        unsigned size;
        double gainPerByte;
    };
    std::vector<Candidate> candidates;

    for (llvm::GlobalVariable *gv : referenced) {
        // Skip if already fixed
        if (fixedPlacements.count(gv))
            continue;

        unsigned nR = 0, nW = 0;
        for (llvm::BasicBlock *BB : intervalBlocks) {
            nR += state.getLoadCount(BB, gv);
            nW += state.getStoreCount(BB, gv);
        }

        auto [liveStart, liveEnd] =
            computeLivenessFlags(gv, intervalBlocks, state, postIntervalBlocks);

        unsigned varSize = state.getVarSizeBytes(gv);
        double E_sr =
            params.memRestoreEnergyPerByte * varSize * (liveStart ? 1.0 : 0.0) +
            params.memStoreEnergyPerByte * varSize * (liveEnd ? 1.0 : 0.0);
        double gain = params.nvmAccessPenalty * (nR + nW) - E_sr;

        if (gain > 0 && varSize > 0) {
            candidates.push_back({gv, gain, varSize, gain / varSize});
        } else {
            // Negative or zero gain: place in NVM
            alloc.placement[gv] = Placement::NVM;
        }
    }

    // Sort by decreasing gain-to-size ratio
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  return a.gainPerByte > b.gainPerByte;
              });

    // Greedy packing
    unsigned vmOffset = fixedVmBytes;
    for (const auto &c : candidates) {
        if (c.size <= remainingVm) {
            alloc.placement[c.gv] = Placement::VM;
            alloc.vmOffsets[c.gv] = vmOffset;
            vmOffset += c.size;
            remainingVm -= c.size;
        } else {
            alloc.placement[c.gv] = Placement::NVM;
        }
    }

    alloc.vmBytesUsed = vmOffset;

    // Compute and store liveness flags for VM-placed variables
    for (const auto &[gv, p] : alloc.placement) {
        if (p == Placement::VM) {
            alloc.livenessFlags[gv] = computeLivenessFlags(
                gv, intervalBlocks, state, postIntervalBlocks);
        }
    }

    return alloc;
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

    double E_restore = 0.0;
    double E_save = 0.0;
    double E_exec = 0.0;

    auto getLiveness = [&](llvm::GlobalVariable *GV) {
        auto it = allocation.livenessFlags.find(GV);
        if (it != allocation.livenessFlags.end())
            return it->second;
        return computeLivenessFlags(
            GV, intervalBlocks, state, postIntervalBlocks);
    };

    // Compute restore cost at interval start
    if (!isFirstInterval) {
        // Normal interval: full restore (E_pro + registers + variables)
        E_restore = params.E_pro +
                    params.N_reg * params.regRestoreEnergy;
        for (const auto &[gv, p] : allocation.placement) {
            if (p == Placement::VM) {
                auto [liveStart, liveEnd] = getLiveness(gv);
                (void)liveEnd;
                if (liveStart) {
                    E_restore += params.memRestoreEnergyPerByte *
                                 state.getVarSizeBytes(gv);
                }
            }
        }
    } else {
        // Function entry: prologue energy only (no register/variable restore)
        E_restore = params.E_pro;
    }

    // Compute save cost at interval end
    if (!isLastInterval) {
        // Normal interval: full save (E_epi + registers + variables)
        E_save = params.E_epi +
                 params.N_reg * params.regStoreEnergy;
        for (const auto &[gv, p] : allocation.placement) {
            if (p == Placement::VM) {
                auto [liveStart, liveEnd] = getLiveness(gv);
                (void)liveStart;
                if (liveEnd) {
                    E_save += params.memStoreEnergyPerByte *
                              state.getVarSizeBytes(gv);
                }
            }
        }
    } else {
        // Function exit: epilogue energy only (no register/variable save)
        E_save = params.E_epi;
    }

    // Compute execution energy: base + NVM access penalties
    for (llvm::BasicBlock *BB : intervalBlocks) {
        E_exec += cfg.getBlockInfo(BB).energyCost;

        // Add NVM access penalty for NVM-placed candidates
        for (const auto &[gv, p] : allocation.placement) {
            if (p == Placement::NVM) {
                unsigned accesses = state.getLoadCount(BB, gv) +
                                    state.getStoreCount(BB, gv);
                E_exec += accesses * params.nvmAccessPenalty;
            }
        }
    }

    return E_restore + E_exec + E_save;
}

} // namespace checkpoint
