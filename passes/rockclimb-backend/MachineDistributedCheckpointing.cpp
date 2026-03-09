#include "MachineDistributedCheckpointing.h"

#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"

using namespace llvm;

namespace checkpoint {

MachineDistributedCheckpointing::MachineDistributedCheckpointing(
    const std::vector<MachineRegionInfo> &regions, const MachineFunction &MF)
    : regions_(regions), MF_(MF) {}

bool MachineDistributedCheckpointing::isCheckpointableReg(MCPhysReg /*reg*/) {
    // Actual filtering done in analyze() using MRI.isReserved()
    return true;
}

unsigned MachineDistributedCheckpointing::assignRegId(MCPhysReg reg) {
    auto it = regIdMap_.find(reg);
    if (it != regIdMap_.end())
        return it->second;
    unsigned id = nextRegId_++;
    regIdMap_[reg] = id;
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
                if (MO.isReg() && MO.isDef() && MO.getReg().isPhysical()) {
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
