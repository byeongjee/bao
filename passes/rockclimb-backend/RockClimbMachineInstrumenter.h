#pragma once

#include "MSP430Constants.h"
#include "MachineDistributedCheckpointing.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/GlobalVariable.h"

#include <vector>

namespace checkpoint {

/// Inserts machine-level checkpoint instructions for MSP430.
///
/// At region boundaries: CALL __region_boundary
/// At register saves:    MOV16mr physReg, &__nvm_regs[regId]
///
/// The debug counter (cnt_boundary) is maintained by rockclimb_boot.S
/// under DEVICE_DEBUG, not by inserted code.
class RockClimbMachineInstrumenter {
  public:
    explicit RockClimbMachineInstrumenter(llvm::MachineFunction &MF,
                                          llvm::GlobalVariable *nvmRegsGV);

    /// Insert boundary check call at the start of MBB
    void insertBoundaryCheck(llvm::MachineBasicBlock &MBB);

    /// Insert the function-entry boundary plus saves of the function's live-in
    /// argument registers. The arg saves precede the boundary call so that
    /// recovery into the callee's first region restores correct argument values
    /// (the per-function distributed analysis cannot reach the caller).
    void insertEntryBoundary(llvm::MachineBasicBlock &entryMBB);

    /// Insert inline register save after the given instruction
    void insertRegisterCheckpoint(const MachineCheckpointPoint &ckpt);

    /// Instrument the function with all boundaries and checkpoints.
    /// Returns the number of instrumentation points inserted.
    unsigned instrument(const std::vector<llvm::MachineBasicBlock *> &boundaries,
                        const std::vector<MachineCheckpointPoint> &checkpoints,
                        bool enableDistributedCkpt);

  private:
    /// Insert CALL __region_boundary at the top of MBB (after any PHIs) and
    /// mark caller-saved R11-R15 clobbered. Returns the call instruction.
    llvm::MachineInstr *emitBoundaryCall(llvm::MachineBasicBlock &MBB);

    llvm::MachineFunction &MF_;
    const llvm::TargetInstrInfo *TII_;
    MSP430Constants C_;
    llvm::GlobalVariable *nvmRegsGV_;
};

} // namespace checkpoint
