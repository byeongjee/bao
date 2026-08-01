#include "MachineDistributedCheckpointing.h"
#include "MachineLivenessAnalysis.h"
#include "RockClimbMachinePass.h"

#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"

#include <cassert>
#include <set>

using namespace llvm;

namespace checkpoint {

MachineDistributedCheckpointing::MachineDistributedCheckpointing(
    const std::vector<MachineRegionInfo> &regions, const MachineFunction &MF)
    : regions_(regions), MF_(MF), C_(MSP430Constants::resolve(MF)) {}

bool MachineDistributedCheckpointing::isCheckpointableReg(MCPhysReg reg) const {
    return reg >= C_.R4.id() && reg <= C_.R15.id();
}

/// Normalize a register (possibly an 8-bit sub-register) to its 16-bit GPR.
/// E.g., R4B → R4.  Returns 0 if the register doesn't overlap any GPR.
static MCPhysReg normalizeToGPR16(MCPhysReg reg, const MachineFunction &MF,
                                  const MSP430Constants &C) {
    if (reg >= C.R4.id() && reg <= C.R15.id())
        return reg; // Already a 16-bit GPR
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    for (unsigned gpr = C.R4.id(); gpr <= C.R15.id(); ++gpr) {
        if (TRI->regsOverlap(reg, gpr))
            return static_cast<MCPhysReg>(gpr);
    }
    return 0;
}

unsigned MachineDistributedCheckpointing::assignRegId(MCPhysReg reg) {
    // Normalize sub-registers (e.g., 8-bit R4B) to their 16-bit GPR
    MCPhysReg gpr = normalizeToGPR16(reg, MF_, C_);
    assert(gpr && "Register does not map to any GPR R4-R15");

    auto it = regIdMap_.find(gpr);
    if (it != regIdMap_.end())
        return it->second;
    // Deterministic mapping: R4=0, R5=1, ..., R15=11
    // This matches boot.S's fixed restore order (__nvm_regs[0] → R4, etc.)
    unsigned id = static_cast<unsigned>(gpr) - C_.R4.id();
    assert(id < 12 && "Register ID out of range for __nvm_regs");
    regIdMap_[gpr] = id;
    return id;
}

static bool instructionUsesOrDefsReg(const MachineInstr &MI, MCPhysReg reg,
                                     const TargetRegisterInfo *TRI, bool &hasDef) {
    bool hasUse = false;
    hasDef = false;
    for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isReg() || !MO.getReg().isPhysical())
            continue;
        if (!TRI->regsOverlap(MO.getReg(), reg))
            continue;
        if (MO.isUse() && !MO.isUndef())
            hasUse = true;
        if (MO.isDef())
            hasDef = true;
    }
    return hasUse || hasDef;
}

/// True if calling this instruction's target may overwrite __nvm_regs slots:
/// the callee is a function this pass instruments (defined in the module and
/// not a runtime helper), or an indirect call whose target is unknown.
/// External declarations (memcpy, __mspabi_*, libm) are never instrumented.
static bool callMayClobberNvmRegs(const MachineInstr &MI) {
    for (const MachineOperand &MO : MI.operands()) {
        if (MO.isGlobal()) {
            const auto *F = dyn_cast<Function>(MO.getGlobal());
            if (!F)
                return true; // alias/unknown target — assume instrumented
            return !F->isDeclaration() && !isRuntimeFunction(F->getName());
        }
        if (MO.isSymbol())
            return false; // lowered libcall — never instrumented
    }
    return true; // indirect call: unknown target may be instrumented
}

static bool reverseTransferBlock(const MachineBasicBlock &MBB, MCPhysReg reg,
                                 const TargetRegisterInfo *TRI, bool needAfterBlock,
                                 std::vector<MachineCheckpointPoint> *checkpoints,
                                 MachineBasicBlock *regionStart, unsigned regId) {
    bool need = needAfterBlock;

    for (auto It = MBB.instr_rbegin(), End = MBB.instr_rend(); It != End; ++It) {
        const MachineInstr &MI = *It;
        bool hasDef = false;
        bool touchesReg = instructionUsesOrDefsReg(MI, reg, TRI, hasDef);

        // __nvm_regs is shared by every instrumented function: a callee's own
        // distributed saves overwrite the caller's slots even though the
        // register values themselves are preserved (callee-saved ABI). Treat
        // such calls as defs so the still-correct register value is re-saved
        // after the call and earlier definitions stop being candidates.
        if (MI.isCall() && callMayClobberNvmRegs(MI))
            hasDef = true;

        if (!touchesReg && !hasDef)
            continue;

        if (hasDef && need && checkpoints != nullptr && !MI.isTerminator()) {
            MachineCheckpointPoint ckpt;
            ckpt.afterInst = const_cast<MachineInstr *>(&MI);
            ckpt.reg = reg;
            ckpt.regId = regId;
            ckpt.regionStart = regionStart;
            checkpoints->push_back(ckpt);
        }

        if (hasDef) {
            // We want the last definition whose value can still reach the
            // boundary. Once we have seen any later definition, earlier defs
            // are no longer checkpoint candidates. Earlier values may still
            // be used to compute this one, but they will be recomputed after
            // recovery from the region start.
            need = false;
        }
    }

    return need;
}

void MachineDistributedCheckpointing::findLastReachingDefs(
    const SmallPtrSetImpl<const MachineBasicBlock *> &predBlockSet, MCPhysReg reg,
    bool boundaryLive, MachineBasicBlock *regionStart,
    std::vector<MachineCheckpointPoint> &checkpoints) {
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    unsigned regId = assignRegId(reg);
    DenseMap<const MachineBasicBlock *, bool> needIn;
    for (const MachineBasicBlock &MBB : MF_) {
        if (!predBlockSet.count(&MBB))
            continue;
        needIn[&MBB] = false;
    }

    bool changed = true;
    while (changed) {
        changed = false;

        for (const MachineBasicBlock &MBB : MF_) {
            if (!predBlockSet.count(&MBB))
                continue;

            bool needOut = false;
            for (const MachineBasicBlock *Succ : MBB.successors()) {
                if (predBlockSet.count(Succ) && needIn.lookup(Succ)) {
                    needOut = true;
                    break;
                }
            }

            bool newNeedIn =
                reverseTransferBlock(MBB, reg, TRI, needOut, nullptr, regionStart, regId);
            if (&MBB == regionStart)
                newNeedIn = boundaryLive || newNeedIn;

            if (needIn.lookup(&MBB) != newNeedIn) {
                needIn[&MBB] = newNeedIn;
                changed = true;
            }
        }
    }

    for (const MachineBasicBlock &MBB : MF_) {
        if (!predBlockSet.count(&MBB))
            continue;

        bool needOut = false;
        for (const MachineBasicBlock *Succ : MBB.successors()) {
            if (predBlockSet.count(Succ) && needIn.lookup(Succ)) {
                needOut = true;
                break;
            }
        }

        reverseTransferBlock(MBB, reg, TRI, needOut, &checkpoints, regionStart, regId);
    }
}

std::vector<MachineCheckpointPoint> MachineDistributedCheckpointing::analyze() {
    std::vector<MachineCheckpointPoint> checkpoints;
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    const MachineRegisterInfo &MRI = MF_.getRegInfo();

    // Build set of all boundary start blocks
    SmallPtrSet<const MachineBasicBlock *, 16> boundarySet;
    for (const MachineRegionInfo &region : regions_)
        boundarySet.insert(region.startBlock);

    for (const MachineRegionInfo &region : regions_) {
        // Step 1: Backward walk — collect blocks that can reach this
        // boundary without passing through another boundary.
        // Include the boundary's own start block: registers defined there
        // (before the boundary CALL that will be inserted later) must also
        // be saved, otherwise they get restored to stale values on recovery.
        SmallPtrSet<const MachineBasicBlock *, 16> predBlockSet;
        {
            predBlockSet.insert(region.startBlock);

            SmallVector<MachineBasicBlock *, 16> worklist;
            for (MachineBasicBlock *pred : region.startBlock->predecessors())
                worklist.push_back(pred);

            while (!worklist.empty()) {
                MachineBasicBlock *MBB = worklist.pop_back_val();
                if (!predBlockSet.insert(MBB).second)
                    continue;
                if (boundarySet.count(MBB))
                    continue; // include but don't walk past
                for (MachineBasicBlock *pred : MBB->predecessors())
                    worklist.push_back(pred);
            }
        }

        // Step 2: Collect all physical registers defined in predecessor blocks
        SmallSet<MCPhysReg, 16> defsInPreds;
        for (const MachineBasicBlock &MBB : MF_) {
            if (!predBlockSet.count(&MBB))
                continue;
            for (const MachineInstr &MI : MBB) {
                for (const MachineOperand &MO : MI.operands()) {
                    if (!MO.isReg() || !MO.isDef() || !MO.getReg().isPhysical())
                        continue;
                    MCPhysReg reg = MO.getReg().asMCReg();
                    if (MRI.isReserved(reg))
                        continue;
                    MCPhysReg gpr = normalizeToGPR16(reg, MF_, C_);
                    if (!gpr || !isCheckpointableReg(gpr))
                        continue;
                    defsInPreds.insert(gpr);
                }
            }
        }

        // Step 3: For each register live at the recovery point, select
        // only last-reaching definitions in predecessor blocks.
        // Liveness is checked from the start of the block because the
        // boundary CALL (inserted later by the instrumenter) goes at
        // the top — all original instructions follow it and re-execute
        // on recovery.
        for (MCPhysReg reg : defsInPreds) {
            bool boundaryLive = isRegLiveFromBlock(region.startBlock, reg, TRI);
            if (!boundaryLive)
                continue;
            findLastReachingDefs(predBlockSet, reg, boundaryLive, region.startBlock, checkpoints);
        }
    }

    std::set<std::pair<const MachineInstr *, unsigned>> seen;
    std::vector<MachineCheckpointPoint> uniqueCheckpoints;
    uniqueCheckpoints.reserve(checkpoints.size());
    for (const MachineCheckpointPoint &ckpt : checkpoints) {
        auto key = std::make_pair(static_cast<const MachineInstr *>(ckpt.afterInst),
                                  static_cast<unsigned>(ckpt.reg));
        if (!seen.insert(key).second)
            continue;
        uniqueCheckpoints.push_back(ckpt);
    }
    checkpoints.swap(uniqueCheckpoints);

    return checkpoints;
}

} // namespace checkpoint
