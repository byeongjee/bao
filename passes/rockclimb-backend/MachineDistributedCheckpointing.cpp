#include "MachineDistributedCheckpointing.h"
#include "MSP430Opcodes.h"
#include "MachineLivenessAnalysis.h"

#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

#include <cassert>

using namespace llvm;

namespace checkpoint {

MachineDistributedCheckpointing::MachineDistributedCheckpointing(
    const std::vector<MachineRegionInfo> &regions, const MachineFunction &MF)
    : regions_(regions), MF_(MF) {}

bool MachineDistributedCheckpointing::isCheckpointableReg(MCPhysReg /*reg*/) {
    // Actual filtering done in analyze() using MRI.isReserved()
    return true;
}

/// Normalize a register (possibly an 8-bit sub-register) to its 16-bit GPR.
/// E.g., R4B → R4.  Returns 0 if the register doesn't overlap any GPR.
static MCPhysReg normalizeToGPR16(MCPhysReg reg, const MachineFunction &MF) {
    if (reg >= msp430::R4 && reg <= msp430::R15)
        return reg; // Already a 16-bit GPR
    const TargetRegisterInfo *TRI = MF.getSubtarget().getRegisterInfo();
    for (unsigned gpr = msp430::R4; gpr <= msp430::R15; ++gpr) {
        if (TRI->regsOverlap(reg, gpr))
            return static_cast<MCPhysReg>(gpr);
    }
    return 0;
}

unsigned MachineDistributedCheckpointing::assignRegId(MCPhysReg reg) {
    // Normalize sub-registers (e.g., 8-bit R4B) to their 16-bit GPR
    MCPhysReg gpr = normalizeToGPR16(reg, MF_);
    assert(gpr && "Register does not map to any GPR R4-R15");

    auto it = regIdMap_.find(gpr);
    if (it != regIdMap_.end())
        return it->second;
    // Deterministic mapping: R4=0, R5=1, ..., R15=11
    // This matches boot.S's fixed restore order (__nvm_regs[0] → R4, etc.)
    unsigned id = static_cast<unsigned>(gpr) - msp430::R4;
    assert(id < 12 && "Register ID out of range for __nvm_regs");
    regIdMap_[gpr] = id;
    return id;
}

void MachineDistributedCheckpointing::findAllDefs(
    const SmallPtrSetImpl<const MachineBasicBlock *> &predBlockSet, MCPhysReg reg,
    MachineBasicBlock *regionStart, std::vector<MachineCheckpointPoint> &checkpoints) {
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    for (const MachineBasicBlock &MBB : MF_) {
        if (!predBlockSet.count(&MBB))
            continue;
        for (const MachineInstr &MI : MBB) {
            if (MI.isTerminator())
                continue;
            for (const MachineOperand &MO : MI.operands()) {
                if (MO.isReg() && MO.isDef() && !MO.isDead() && MO.getReg().isPhysical() &&
                    TRI->regsOverlap(MO.getReg(), reg)) {
                    MachineCheckpointPoint ckpt;
                    ckpt.afterInst = const_cast<MachineInstr *>(&MI);
                    ckpt.reg = reg;
                    ckpt.regId = assignRegId(reg);
                    ckpt.regionStart = regionStart;
                    checkpoints.push_back(ckpt);
                    break; // one save per instruction
                }
            }
        }
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
                    defsInPreds.insert(reg);
                }
            }
        }

        // Step 3: Find boundary CALL in startBlock (recovery point)
        const MachineInstr *boundaryCall = nullptr;
        for (const MachineInstr &MI : *region.startBlock) {
            if (MI.isCall()) {
                boundaryCall = &MI;
                break;
            }
        }

        // Step 4: For each register live at the recovery point, create
        // saves at EVERY definition in predecessor blocks.
        for (MCPhysReg reg : defsInPreds) {
            if (!isRegLiveFromBlock(region.startBlock, reg, TRI, boundaryCall))
                continue;
            findAllDefs(predBlockSet, reg, region.startBlock, checkpoints);
        }
    }

    return checkpoints;
}

} // namespace checkpoint
