#include "RockClimbMachineInstrumenter.h"
#include "MachineLivenessAnalysis.h"

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DebugLoc.h"

#include <cassert>

using namespace llvm;

namespace checkpoint {

RockClimbMachineInstrumenter::RockClimbMachineInstrumenter(MachineFunction &MF,
                                                           GlobalVariable *nvmRegsGV)
    : MF_(MF), TII_(MF.getSubtarget().getInstrInfo()), C_(MSP430Constants::resolve(MF)),
      nvmRegsGV_(nvmRegsGV) {}

MachineInstr *RockClimbMachineInstrumenter::emitBoundaryCall(MachineBasicBlock &MBB) {
    // INVARIANT: recovery restores R4-R15/SP/PC but NOT SR, so a region-start
    // block must never consume status flags produced before the boundary.
    // This holds at -stop-after=virtregrewriter (compare and conditional
    // branch are still colocated per block) but would silently break if this
    // pass ever moved after branch folding, which creates SR-live-in blocks.
    //
    // Insert at the beginning of the block, after any PHI-like copies
    // (shouldn't exist post-regalloc, but be safe).
    MachineBasicBlock::iterator InsertPt = MBB.begin();
    while (InsertPt != MBB.end() && InsertPt->isPHI())
        ++InsertPt;

    DebugLoc DL;
    if (InsertPt != MBB.end())
        DL = InsertPt->getDebugLoc();

    MachineInstr *CallMI =
        BuildMI(MBB, InsertPt, DL, TII_->get(C_.CALLi)).addExternalSymbol("__region_boundary");

    // Mark caller-saved registers (R11-R15) as implicitly clobbered by the
    // call. Post-regalloc, we must declare which registers the call may
    // overwrite.
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    for (unsigned reg = C_.R11.id(); reg <= C_.R15.id(); ++reg)
        CallMI->addRegisterDefined(reg, TRI);

    return CallMI;
}

void RockClimbMachineInstrumenter::insertBoundaryCheck(MachineBasicBlock &MBB) {
    emitBoundaryCall(MBB);

    // cnt_boundary and cnt_restore_reg are counted in assembly
    // (rockclimb_boot.S) under #ifdef DEVICE_DEBUG, consistent with
    // MILP and SCHEMATIC.
}

void RockClimbMachineInstrumenter::insertEntryBoundary(MachineBasicBlock &entryMBB) {
    // Determine the function's live-in (argument) registers BEFORE inserting the
    // boundary call (which marks R11–R15 clobbered). A register is a live-in arg
    // if it is read before being written from the entry block. isRegLiveFromBlock
    // correctly treats a two-address read-modify-write (e.g. `$r12 = ADD $r12,
    // $r13`) as a use of $r12, which a naive def-first scan would miss. Locals
    // are written before read (not live-in) and are preserved across power loss
    // via the FRAM stack, so this set is exactly the register arguments.
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    SmallVector<MCPhysReg, 8> argRegs;
    for (unsigned r = C_.R4.id(); r <= C_.R15.id(); ++r) {
        MCPhysReg reg = static_cast<MCPhysReg>(r);
        if (isRegLiveFromBlock(&entryMBB, reg, TRI))
            argRegs.push_back(reg);
    }

    // Boundary checkpoint at function entry (recovery point for the callee's
    // first region; bounds the caller→callee span together with the caller's
    // own region cutting).
    MachineInstr *CallMI = emitBoundaryCall(entryMBB);

    // Save the function's live-in (argument) registers immediately BEFORE the
    // boundary call, so recovery into the entry region restores correct args.
    DebugLoc DL = CallMI->getDebugLoc();
    MachineBasicBlock::iterator SavePt(CallMI); // saves go before the boundary
    for (MCPhysReg reg : argRegs) {
        unsigned regId = reg - C_.R4.id();
        BuildMI(entryMBB, SavePt, DL, TII_->get(C_.MOV16mr))
            .addReg(C_.SR) // base = SR (absolute addressing)
            .addGlobalAddress(nvmRegsGV_, static_cast<int64_t>(regId) * 2)
            .addReg(reg);
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

    // The analysis normalizes sub-registers to their 16-bit GPR.
    assert(ckpt.reg >= C_.R4.id() && ckpt.reg <= C_.R15.id() &&
           "checkpoint register must be a 16-bit GPR R4-R15");

    // Inline save: MOV16mr physReg, &__nvm_regs[regId]
    // memdst operands: (base=SR for absolute addressing, disp=GlobalAddress+offset)
    BuildMI(*MBB, InsertPt, DL, TII_->get(C_.MOV16mr))
        .addReg(C_.SR) // base = SR (absolute addressing)
        .addGlobalAddress(nvmRegsGV_,
                          static_cast<int64_t>(ckpt.regId) * 2) // disp = &__nvm_regs + regId*2
        .addReg(ckpt.reg);                                      // source register

    // NOTE: No per-save debug counter here on purpose: each flag-preserving
    // increment (PUSH SR / ADD / ADDC / POP SR) bloated large benchmarks
    // past FRAM capacity. Boundary and restore counters are maintained in
    // rockclimb_boot.S under DEVICE_DEBUG.
}

unsigned
RockClimbMachineInstrumenter::instrument(const std::vector<MachineBasicBlock *> &boundaries,
                                         const std::vector<MachineCheckpointPoint> &checkpoints,
                                         bool enableDistributedCkpt) {
    // MSP430 opcode/register constants are resolved by name (and validated) in
    // the constructor via MSP430Constants::resolve, which aborts on any miss.

    unsigned count = 0;

    // Function entry and exit(s) are both region boundaries (PFI model): the
    // entry boundary (with live-in arg saves) bounds the call-in span and the
    // exit boundary bounds the return span. The entry boundary is emitted (no
    // longer skipped) so the callee's first region has a recovery point.
    MachineBasicBlock *entryMBB = &MF_.front();
    for (MachineBasicBlock *MBB : boundaries) {
        if (MBB == entryMBB)
            insertEntryBoundary(*MBB);
        else
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
