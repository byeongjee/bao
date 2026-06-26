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
/// When addDebugMarkers is enabled, also inserts inline NVM counter
/// increments (ADD16mi) for profiling — no function calls, so no
/// post-regalloc register clobber issues.
class RockClimbMachineInstrumenter {
  public:
    explicit RockClimbMachineInstrumenter(llvm::MachineFunction &MF, bool addDebugMarkers,
                                          llvm::GlobalVariable *nvmRegsGV,
                                          llvm::GlobalVariable *cntBoundaryGV,
                                          llvm::GlobalVariable *cntSaveGV,
                                          llvm::GlobalVariable *cntRestoreGV);

    /// Insert boundary check call at the start of MBB
    void insertBoundaryCheck(llvm::MachineBasicBlock &MBB);

    /// Insert inline register save (and optional debug counter) after the given instruction
    void insertRegisterCheckpoint(const MachineCheckpointPoint &ckpt);

    /// Instrument the function with all boundaries and checkpoints.
    /// Returns the number of instrumentation points inserted.
    unsigned instrument(const std::vector<llvm::MachineBasicBlock *> &boundaries,
                        const std::vector<MachineCheckpointPoint> &checkpoints,
                        bool enableDistributedCkpt);

  private:
    /// Emit a flag-safe uint32_t counter increment:
    /// PUSH SR, ADD16mi low word, ADDC16mc high word, POP SR.
    /// Preserves status register flags across the increment.
    void emitCounterIncrement(llvm::MachineBasicBlock &MBB,
                              llvm::MachineBasicBlock::iterator InsertPt, const llvm::DebugLoc &DL,
                              llvm::GlobalVariable *counterGV, int64_t amount);

    llvm::MachineFunction &MF_;
    const llvm::TargetInstrInfo *TII_;
    MSP430Constants C_;
    bool addDebugMarkers_;
    llvm::GlobalVariable *nvmRegsGV_;
    llvm::GlobalVariable *cntBoundaryGV_;
    llvm::GlobalVariable *cntSaveGV_;
    llvm::GlobalVariable *cntRestoreGV_;
};

} // namespace checkpoint
