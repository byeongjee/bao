#include "RockClimbMachineInstrumenter.h"
#include "MSP430Opcodes.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace checkpoint {

RockClimbMachineInstrumenter::RockClimbMachineInstrumenter(MachineFunction &MF)
    : MF_(MF), TII_(MF.getSubtarget().getInstrInfo()) {}

bool RockClimbMachineInstrumenter::verifyConstants() const {
    bool ok = true;

    // Verify opcode names match our hardcoded values
    auto checkOpcode = [&](unsigned opcode, const char *expected) {
        StringRef name = TII_->getName(opcode);
        if (name != expected) {
            errs() << "RockClimbMachineInstrumenter: opcode " << opcode << " is '" << name
                   << "', expected '" << expected << "'\n";
            ok = false;
        }
    };

    checkOpcode(msp430::CALLi, "CALLi");
    checkOpcode(msp430::MOV16ri, "MOV16ri");
    checkOpcode(msp430::MOV16rr, "MOV16rr");

    // Verify register names
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    auto checkReg = [&](unsigned reg, const char *expected) {
        StringRef name = TRI->getName(reg);
        if (name != expected) {
            errs() << "RockClimbMachineInstrumenter: register " << reg << " is '" << name
                   << "', expected '" << expected << "'\n";
            ok = false;
        }
    };

    checkReg(msp430::R12, "R12");
    checkReg(msp430::R14, "R14");

    return ok;
}

void RockClimbMachineInstrumenter::insertBoundaryCheck(MachineBasicBlock &MBB) {
    // Insert CALL __rockclimb_check at the beginning of the block,
    // after any PHI-like copies at the start.
    MachineBasicBlock::iterator InsertPt = MBB.begin();

    // Skip past any PHI nodes (shouldn't exist post-regalloc, but be safe)
    while (InsertPt != MBB.end() && InsertPt->isPHI())
        ++InsertPt;

    DebugLoc DL;
    if (InsertPt != MBB.end())
        DL = InsertPt->getDebugLoc();

    // CALL __rockclimb_check
    MachineInstr *CallMI =
        BuildMI(MBB, InsertPt, DL, TII_->get(msp430::CALLi)).addExternalSymbol("__rockclimb_check");

    // Mark caller-saved registers as implicitly defined/clobbered by the call.
    // Post-regalloc, we must declare which registers the call may overwrite.
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    // MSP430 caller-saved: R11-R15
    for (unsigned reg : {msp430::R11, msp430::R12, msp430::R13, msp430::R14, msp430::R15}) {
        CallMI->addRegisterDefined(reg, TRI);
    }
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

    // Insert: MOV #regId, R12  (first argument: register slot index)
    BuildMI(*MBB, InsertPt, DL, TII_->get(msp430::MOV16ri), msp430::R12).addImm(ckpt.regId);

    // Insert: MOV physReg, R14  (second argument: register value)
    // MSP430 calling convention: R12=arg1, R14=arg2
    // If the register is an 8-bit sub-register (GR8), promote to its 16-bit
    // super-register. Saving the full 16-bit register is safe — the 8-bit value
    // lives in the low byte.
    MCRegister srcReg = ckpt.reg;
    const TargetRegisterClass *RC = TRI->getMinimalPhysRegClass(srcReg);
    if (RC && RC->getID() == msp430::GR8RegClassID) {
        const TargetRegisterClass *GR16RC = TRI->getRegClass(msp430::GR16RegClassID);
        MCRegister superReg = TRI->getMatchingSuperReg(srcReg, msp430::subreg_8bit, GR16RC);
        if (superReg)
            srcReg = superReg;
    }
    BuildMI(*MBB, InsertPt, DL, TII_->get(msp430::MOV16rr), msp430::R14).addReg(srcReg);

    // Insert: CALL __rockclimb_save_reg
    MachineInstr *CallMI = BuildMI(*MBB, InsertPt, DL, TII_->get(msp430::CALLi))
                               .addExternalSymbol("__rockclimb_save_reg");

    // Mark caller-saved registers as clobbered
    for (unsigned reg : {msp430::R11, msp430::R12, msp430::R13, msp430::R14, msp430::R15}) {
        CallMI->addRegisterDefined(reg, TRI);
    }
}

unsigned
RockClimbMachineInstrumenter::instrument(const std::vector<MachineBasicBlock *> &boundaries,
                                         const std::vector<MachineCheckpointPoint> &checkpoints,
                                         bool enableDistributedCkpt) {

    if (!verifyConstants()) {
        errs() << "WARNING: MSP430 opcode/register constants mismatch. "
               << "Instrumentation may produce incorrect code.\n";
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
