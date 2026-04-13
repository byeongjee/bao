#include "RockClimbMachineInstrumenter.h"
#include "MSP430Opcodes.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/Support/raw_ostream.h"

#include "common/Logger.h"

using namespace llvm;

namespace checkpoint {

RockClimbMachineInstrumenter::RockClimbMachineInstrumenter(
    MachineFunction &MF, bool addDebugMarkers, GlobalVariable *nvmRegsGV,
    GlobalVariable *cntBoundaryGV, GlobalVariable *cntSaveGV, GlobalVariable *cntRestoreGV)
    : MF_(MF), TII_(MF.getSubtarget().getInstrInfo()), addDebugMarkers_(addDebugMarkers),
      nvmRegsGV_(nvmRegsGV), cntBoundaryGV_(cntBoundaryGV), cntSaveGV_(cntSaveGV),
      cntRestoreGV_(cntRestoreGV) {}

bool RockClimbMachineInstrumenter::verifyConstants() const {
    bool ok = true;

    // Verify opcode names match our hardcoded values
    auto checkOpcode = [&](unsigned opcode, const char *expected) {
        StringRef name = TII_->getName(opcode);
        if (name != expected) {
            PLOGD << "RockClimbMachineInstrumenter: opcode " << opcode << " is '" << name
                  << "', expected '" << expected << "'";
            ok = false;
        }
    };

    checkOpcode(msp430::CALLi, "CALLi");
    checkOpcode(msp430::MOV16mr, "MOV16mr");
    checkOpcode(msp430::ADD16mi, "ADD16mi");
    checkOpcode(msp430::ADDC16mc, "ADDC16mc");
    checkOpcode(msp430::PUSH16r, "PUSH16r");
    checkOpcode(msp430::POP16r, "POP16r");

    return ok;
}

void RockClimbMachineInstrumenter::emitCounterIncrement(MachineBasicBlock &MBB,
                                                        MachineBasicBlock::iterator InsertPt,
                                                        const DebugLoc &DL,
                                                        GlobalVariable *counterGV, int64_t amount) {
    // Wrap the 32-bit increment with PUSH SR / POP SR to preserve status flags.
    // ADD16mi and ADDC16mc clobber SR (V, N, Z, C), which can break
    // conditional branches that depend on flags set by earlier instructions.
    BuildMI(MBB, InsertPt, DL, TII_->get(msp430::PUSH16r)).addReg(msp430::SR);

    BuildMI(MBB, InsertPt, DL, TII_->get(msp430::ADD16mi))
        .addReg(msp430::SR)          // base = SR (absolute addressing)
        .addGlobalAddress(counterGV) // address of counter
        .addImm(amount);

    BuildMI(MBB, InsertPt, DL, TII_->get(msp430::ADDC16mc))
        .addReg(msp430::SR)             // base = SR (absolute addressing)
        .addGlobalAddress(counterGV, 2) // high half of uint32_t counter
        .addImm(0);

    BuildMI(MBB, InsertPt, DL, TII_->get(msp430::POP16r)).addReg(msp430::SR, RegState::Define);
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
        BuildMI(MBB, InsertPt, DL, TII_->get(msp430::CALLi)).addExternalSymbol("__region_boundary");

    // Mark caller-saved registers as implicitly defined/clobbered by the call.
    // Post-regalloc, we must declare which registers the call may overwrite.
    // MSP430 caller-saved: R11-R15
    for (unsigned reg : {msp430::R11, msp430::R12, msp430::R13, msp430::R14, msp430::R15}) {
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
    if (RC && RC->getID() == msp430::GR8RegClassID) {
        MCPhysReg superReg = TRI->getMatchingSuperReg(srcReg, msp430::subreg_8bit,
                                                      TRI->getMinimalPhysRegClass(msp430::R4));
        if (superReg)
            srcReg = superReg;
    }

    // Inline save: MOV16mr physReg, &__nvm_regs[regId]
    // memdst operands: (base=SR for absolute addressing, disp=GlobalAddress+offset)
    BuildMI(*MBB, InsertPt, DL, TII_->get(msp430::MOV16mr))
        .addReg(msp430::SR) // base = SR (absolute addressing)
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

    if (!verifyConstants()) {
        PLOGW << "WARNING: MSP430 opcode/register constants mismatch. "
              << "Instrumentation may produce incorrect code.";
    }

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
