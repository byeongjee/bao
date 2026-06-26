#include "RockClimbMachineInstrumenter.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DebugLoc.h"

using namespace llvm;

namespace checkpoint {

RockClimbMachineInstrumenter::RockClimbMachineInstrumenter(
    MachineFunction &MF, bool addDebugMarkers, GlobalVariable *nvmRegsGV,
    GlobalVariable *cntBoundaryGV, GlobalVariable *cntSaveGV, GlobalVariable *cntRestoreGV)
    : MF_(MF), TII_(MF.getSubtarget().getInstrInfo()), C_(MSP430Constants::resolve(MF)),
      addDebugMarkers_(addDebugMarkers), nvmRegsGV_(nvmRegsGV), cntBoundaryGV_(cntBoundaryGV),
      cntSaveGV_(cntSaveGV), cntRestoreGV_(cntRestoreGV) {}

void RockClimbMachineInstrumenter::emitCounterIncrement(MachineBasicBlock &MBB,
                                                        MachineBasicBlock::iterator InsertPt,
                                                        const DebugLoc &DL,
                                                        GlobalVariable *counterGV, int64_t amount) {
    // Wrap the 32-bit increment with PUSH SR / POP SR to preserve status flags.
    // ADD16mi and ADDC16mc clobber SR (V, N, Z, C), which can break
    // conditional branches that depend on flags set by earlier instructions.
    BuildMI(MBB, InsertPt, DL, TII_->get(C_.PUSH16r)).addReg(C_.SR);

    BuildMI(MBB, InsertPt, DL, TII_->get(C_.ADD16mi))
        .addReg(C_.SR)               // base = SR (absolute addressing)
        .addGlobalAddress(counterGV) // address of counter
        .addImm(amount);

    BuildMI(MBB, InsertPt, DL, TII_->get(C_.ADDC16mc))
        .addReg(C_.SR)                  // base = SR (absolute addressing)
        .addGlobalAddress(counterGV, 2) // high half of uint32_t counter
        .addImm(0);

    BuildMI(MBB, InsertPt, DL, TII_->get(C_.POP16r)).addReg(C_.SR, RegState::Define);
}

void RockClimbMachineInstrumenter::insertBoundaryCheck(MachineBasicBlock &MBB) {
    // Insert CALL __region_boundary at the beginning of the block,
    // after any PHI-like copies at the start.
    MachineBasicBlock::iterator InsertPt = MBB.begin();

    // Skip past any PHI nodes (shouldn't exist post-regalloc, but be safe)
    while (InsertPt != MBB.end() && InsertPt->isPHI())
        ++InsertPt;

    DebugLoc DL;
    if (InsertPt != MBB.end())
        DL = InsertPt->getDebugLoc();

    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();

    // CALL __region_boundary
    MachineInstr *CallMI =
        BuildMI(MBB, InsertPt, DL, TII_->get(C_.CALLi)).addExternalSymbol("__region_boundary");

    // Mark caller-saved registers as implicitly defined/clobbered by the call.
    // Post-regalloc, we must declare which registers the call may overwrite.
    // MSP430 caller-saved: R11-R15 (contiguous, verified by MSP430Constants).
    for (unsigned reg = C_.R15.id() - 4; reg <= C_.R15.id(); ++reg) {
        CallMI->addRegisterDefined(reg, TRI);
    }

    // cnt_boundary and cnt_restore_reg are counted in assembly
    // (rockclimb_boot.S) under #ifdef DEVICE_DEBUG, consistent with
    // MILP and SCHEMATIC.
}

void RockClimbMachineInstrumenter::insertRegisterCheckpoint(const MachineCheckpointPoint &ckpt) {
    if (!ckpt.afterInst)
        return;

    // Skip if the defining instruction is a terminator
    if (ckpt.afterInst->isTerminator())
        return;

    MachineBasicBlock *MBB = ckpt.afterInst->getParent();
    MachineBasicBlock::iterator InsertPt(ckpt.afterInst);
    ++InsertPt; // Insert AFTER the defining instruction

    DebugLoc DL = ckpt.afterInst->getDebugLoc();
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();

    // Determine the source register for the MOV.
    // If the physical register is GR8, promote to its GR16 super-register.
    unsigned srcReg = ckpt.reg;
    const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(srcReg);
    if (RC && RC->getID() == C_.GR8RegClassID) {
        MCPhysReg superReg =
            TRI->getMatchingSuperReg(srcReg, C_.subreg_8bit, TRI->getMinimalPhysRegClass(C_.R4));
        if (superReg)
            srcReg = superReg;
    }

    // Inline save: MOV16mr physReg, &__nvm_regs[regId]
    // memdst operands: (base=SR for absolute addressing, disp=GlobalAddress+offset)
    BuildMI(*MBB, InsertPt, DL, TII_->get(C_.MOV16mr))
        .addReg(C_.SR) // base = SR (absolute addressing)
        .addGlobalAddress(nvmRegsGV_,
                          static_cast<int64_t>(ckpt.regId) * 2) // disp = &__nvm_regs + regId*2
        .addReg(srcReg);                                        // source register

    // NOTE: Per-save debug counters are disabled to control code size.
    // Each distributed save-point increment injects a flag-preserving
    // PUSH/ADD/ADDC/POP sequence, which bloats large benchmarks enough
    // to overflow FRAM. Keep the register save itself, but skip the
    // counter update.
    //
    // if (addDebugMarkers_ && cntSaveGV_) {
    //     emitCounterIncrement(*MBB, InsertPt, DL, cntSaveGV_, 1);
    // }
}

unsigned
RockClimbMachineInstrumenter::instrument(const std::vector<MachineBasicBlock *> &boundaries,
                                         const std::vector<MachineCheckpointPoint> &checkpoints,
                                         bool enableDistributedCkpt) {
    // MSP430 opcode/register constants are resolved by name (and validated) in
    // the constructor via MSP430Constants::resolve, which aborts on any miss.

    unsigned count = 0;

    // Insert boundary checks (skip entry block — execution just started)
    MachineBasicBlock *entryMBB = &MF_.front();
    for (MachineBasicBlock *MBB : boundaries) {
        if (MBB == entryMBB)
            continue;
        insertBoundaryCheck(*MBB);
        ++count;
    }

    // Insert distributed register checkpoints
    if (enableDistributedCkpt) {
        for (const auto &ckpt : checkpoints) {
            insertRegisterCheckpoint(ckpt);
            ++count;
        }
    }

    return count;
}

} // namespace checkpoint
