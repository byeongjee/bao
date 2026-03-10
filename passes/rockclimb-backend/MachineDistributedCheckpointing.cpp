#include "MachineDistributedCheckpointing.h"
#include "MSP430Opcodes.h"

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

SmallPtrSet<MachineBasicBlock *, 8>
MachineDistributedCheckpointing::makeRegionBlockSet(const MachineRegionInfo &region) const {
    SmallPtrSet<MachineBasicBlock *, 8> blockSet;
    for (MachineBasicBlock *MBB : region.blocks)
        blockSet.insert(MBB);
    return blockSet;
}

MachineInstr *MachineDistributedCheckpointing::findLastDef(const MachineRegionInfo &region,
                                                           MCPhysReg reg) const {
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    MachineInstr *lastDef = nullptr;

    for (MachineBasicBlock *MBB : region.blocks) {
        for (MachineInstr &MI : *MBB) {
            for (const MachineOperand &MO : MI.operands()) {
                if (MO.isReg() && MO.isDef() && !MO.isDead() && MO.getReg().isPhysical()) {
                    if (TRI->regsOverlap(MO.getReg(), reg))
                        lastDef = &MI;
                }
            }
        }
    }

    return lastDef;
}

/// Check if any alias of reg is live-in to the given block
static bool isRegOrAliasLiveIn(const MachineBasicBlock *MBB, MCPhysReg reg,
                               const TargetRegisterInfo *TRI) {
    for (MCRegAliasIterator AI(reg, TRI, /*IncludeSelf=*/true); AI.isValid(); ++AI) {
        if (MBB->isLiveIn(*AI))
            return true;
    }
    return false;
}

std::vector<MachineCheckpointPoint> MachineDistributedCheckpointing::analyze() {
    std::vector<MachineCheckpointPoint> checkpoints;
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    const MachineRegisterInfo &MRI = MF_.getRegInfo();

    for (const MachineRegionInfo &region : regions_) {
        auto regionBlockSet = makeRegionBlockSet(region);

        // Step 1: Collect all physical registers DEFINED in this region
        // Use SmallSet instead of SmallPtrSet (MCPhysReg is a scalar, not pointer)
        SmallSet<MCPhysReg, 16> defsInRegion;
        for (MachineBasicBlock *MBB : region.blocks) {
            for (MachineInstr &MI : *MBB) {
                for (const MachineOperand &MO : MI.operands()) {
                    if (!MO.isReg() || !MO.isDef() || !MO.getReg().isPhysical())
                        continue;
                    MCPhysReg reg = MO.getReg().asMCReg();
                    // Skip reserved registers (PC, SP, SR, CG)
                    if (MRI.isReserved(reg))
                        continue;
                    defsInRegion.insert(reg);
                }
            }
        }

        // Step 2: Find registers that are LIVE-OUT of the region.
        // A register is live-out if it's defined in the region and
        // live-in to a successor block outside the region.
        SmallSet<MCPhysReg, 16> liveOutRegs;
        for (MachineBasicBlock *MBB : region.blocks) {
            for (MachineBasicBlock *succ : MBB->successors()) {
                if (regionBlockSet.count(succ))
                    continue;

                for (MCPhysReg reg : defsInRegion) {
                    if (isRegOrAliasLiveIn(succ, reg, TRI))
                        liveOutRegs.insert(reg);
                }
            }
        }

        // Step 3: For each live-out register, find last def and create
        // checkpoint point
        for (MCPhysReg reg : liveOutRegs) {
            MachineInstr *lastDef = findLastDef(region, reg);
            if (!lastDef)
                continue;

            // Skip if the last def is a terminator (can't insert after it)
            if (lastDef->isTerminator())
                continue;

            MachineCheckpointPoint ckpt;
            ckpt.afterInst = lastDef;
            ckpt.reg = reg;
            ckpt.regId = assignRegId(reg);
            ckpt.regionStart = region.startBlock;
            checkpoints.push_back(ckpt);
        }
    }

    return checkpoints;
}

} // namespace checkpoint
