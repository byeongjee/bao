#pragma once

#include "MachineDistributedCheckpointing.h"

#include "llvm/CodeGen/MachineFunction.h"

#include <vector>

namespace checkpoint {

/// Inserts machine-level checkpoint calls via BuildMI for MSP430.
///
/// At region boundaries: CALL __rockclimb_check
/// At register saves:    MOV #regId, R12; MOV physReg, R14; CALL __rockclimb_save_reg
class RockClimbMachineInstrumenter {
  public:
    explicit RockClimbMachineInstrumenter(llvm::MachineFunction &MF);

    /// Verify that hardcoded MSP430 opcode/register constants match runtime values.
    /// Call once at initialization. Returns false and prints errors if mismatched.
    bool verifyConstants() const;

    /// Insert boundary check call at the start of MBB
    void insertBoundaryCheck(llvm::MachineBasicBlock &MBB);

    /// Insert register save call after the given instruction
    void insertRegisterCheckpoint(const MachineCheckpointPoint &ckpt);

    /// Instrument the function with all boundaries and checkpoints.
    /// Returns the number of instrumentation points inserted.
    unsigned instrument(const std::vector<llvm::MachineBasicBlock *> &boundaries,
                        const std::vector<MachineCheckpointPoint> &checkpoints,
                        bool enableDistributedCkpt);

  private:
    llvm::MachineFunction &MF_;
    const llvm::TargetInstrInfo *TII_;
};

} // namespace checkpoint
