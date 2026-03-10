#pragma once

#include "MachineDistributedCheckpointing.h"

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/IR/GlobalVariable.h"

#include <vector>

namespace checkpoint {

/// Inserts machine-level checkpoint instructions for MSP430.
///
/// At region boundaries: CALL __rockclimb_check
/// At register saves:    MOV16mr physReg, &__nvm_regs[regId]
///                       (+ CALL __rockclimb_save_reg if addDebugMarkers)
class RockClimbMachineInstrumenter {
  public:
    explicit RockClimbMachineInstrumenter(llvm::MachineFunction &MF, bool addDebugMarkers,
                                          llvm::GlobalVariable *nvmRegsGV);

    /// Verify that hardcoded MSP430 opcode/register constants match runtime values.
    /// Call once at initialization. Returns false and prints errors if mismatched.
    bool verifyConstants() const;

    /// Insert boundary check call at the start of MBB
    void insertBoundaryCheck(llvm::MachineBasicBlock &MBB);

    /// Insert inline register save (and optional debug call) after the given instruction
    void insertRegisterCheckpoint(const MachineCheckpointPoint &ckpt);

    /// Instrument the function with all boundaries and checkpoints.
    /// Returns the number of instrumentation points inserted.
    unsigned instrument(const std::vector<llvm::MachineBasicBlock *> &boundaries,
                        const std::vector<MachineCheckpointPoint> &checkpoints,
                        bool enableDistributedCkpt);

  private:
    llvm::MachineFunction &MF_;
    const llvm::TargetInstrInfo *TII_;
    bool addDebugMarkers_;
    llvm::GlobalVariable *nvmRegsGV_;
};

} // namespace checkpoint
