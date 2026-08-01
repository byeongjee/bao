#pragma once

#include "MSP430Constants.h"
#include "RockClimbMachineOptimizer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/MC/MCRegister.h"

#include <vector>

namespace checkpoint {

struct MachineCheckpointPoint {
    llvm::MachineInstr *afterInst = nullptr;
    llvm::MCPhysReg reg = 0; // Physical register (e.g., MSP430::R4)
    unsigned regId = 0;      // Slot index in __nvm_regs[]
    llvm::MachineBasicBlock *regionStart = nullptr;
};

/// Distributed checkpointing on physical registers after register allocation.
///
/// For each region boundary, computes the set of blocks that can reach
/// the boundary without passing through another boundary (backward
/// reachability). For each register live at the recovery point, selects
/// only the last definition sites whose values can still reach that
/// boundary. Distinct predecessor paths may still contribute different
/// last-reaching definitions, but redundant earlier definitions are
/// skipped.
///
/// Only checkpoints callee-saved (R4-R10) and argument/return registers (R11-R15).
/// Skips reserved registers: PC (R0), SP (R1), SR (R2), CG (R3).
class MachineDistributedCheckpointing {
  public:
    explicit MachineDistributedCheckpointing(const std::vector<MachineRegionInfo> &regions,
                                             const llvm::MachineFunction &MF);

    std::vector<MachineCheckpointPoint> analyze();

  private:
    const std::vector<MachineRegionInfo> &regions_;
    const llvm::MachineFunction &MF_;
    MSP430Constants C_;

    /// Map physical register to unique NVM slot ID
    llvm::DenseMap<llvm::MCPhysReg, unsigned> regIdMap_;

    unsigned assignRegId(llvm::MCPhysReg reg);

    /// Check if a physical register should be checkpointed
    bool isCheckpointableReg(llvm::MCPhysReg reg) const;

    /// Create checkpoint points at last-reaching definitions of `reg`
    /// in `predBlockSet`. Only called for registers live at the boundary.
    void
    findLastReachingDefs(const llvm::SmallPtrSetImpl<const llvm::MachineBasicBlock *> &predBlockSet,
                         llvm::MCPhysReg reg, llvm::MachineBasicBlock *regionStart,
                         std::vector<MachineCheckpointPoint> &checkpoints);
};

} // namespace checkpoint
