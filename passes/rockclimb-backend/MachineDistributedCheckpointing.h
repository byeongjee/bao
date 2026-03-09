#pragma once

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
/// For each region, identifies physical registers that are:
///   1. Defined within the region
///   2. Live-out of the region (used in subsequent regions)
/// Then places a checkpoint at the last definition point within the region.
///
/// Only checkpoints callee-saved (R4-R10) and argument/return registers (R11-R15).
/// Skips reserved registers: PC (R0), SP (R1), SR (R2), CG (R3).
class MachineDistributedCheckpointing {
  public:
    explicit MachineDistributedCheckpointing(const std::vector<MachineRegionInfo> &regions,
                                             const llvm::MachineFunction &MF);

    std::vector<MachineCheckpointPoint> analyze();

    unsigned getCheckpointedRegisterCount() const { return nextRegId_; }

  private:
    const std::vector<MachineRegionInfo> &regions_;
    const llvm::MachineFunction &MF_;
    unsigned nextRegId_ = 0;

    /// Map physical register to unique NVM slot ID
    llvm::DenseMap<llvm::MCPhysReg, unsigned> regIdMap_;

    unsigned assignRegId(llvm::MCPhysReg reg);

    /// Check if a physical register should be checkpointed
    static bool isCheckpointableReg(llvm::MCPhysReg reg);

    /// Collect physical registers defined in a region
    llvm::SmallPtrSet<llvm::MachineBasicBlock *, 8>
    makeRegionBlockSet(const MachineRegionInfo &region) const;

    /// Find the last instruction in the region that defines the given register
    llvm::MachineInstr *findLastDef(const MachineRegionInfo &region, llvm::MCPhysReg reg) const;
};

} // namespace checkpoint
