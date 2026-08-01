#pragma once

#include "MSP430Constants.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/MC/MCRegister.h"

#include <string>
#include <vector>

namespace checkpoint {

class MachineEnergyEstimator;

struct MachineRegionInfo {
    llvm::MachineBasicBlock *startBlock = nullptr;
    std::vector<llvm::MachineBasicBlock *> blocks;
    double totalEnergy = 0.0;
};

struct MachineRockClimbResult {
    std::vector<llvm::MachineBasicBlock *> regionBoundaries;
    std::vector<MachineRegionInfo> regions;
    bool feasible = true;
    std::string errorMessage;
};

/// Algorithm 1 (region partitioning) operating on MachineFunction.
/// Direct port of RockClimbOptimizer from IR level to machine level.
class RockClimbMachineOptimizer {
  public:
    RockClimbMachineOptimizer(llvm::MachineFunction &MF, llvm::MachineLoopInfo &MLI,
                              const MachineEnergyEstimator &estimator, double E_safe,
                              double reg_store_energy);

    MachineRockClimbResult optimize();

  private:
    llvm::MachineFunction &MF_;
    llvm::MachineLoopInfo &MLI_;
    const MachineEnergyEstimator &estimator_;
    double E_safe_;

    /// Per-block energy costs (computed from estimator, may include extras)
    llvm::DenseMap<llvm::MachineBasicBlock *, double> energyCosts_;

    /// Loop headers (mandatory region boundaries)
    llvm::SmallPtrSet<llvm::MachineBasicBlock *, 16> loopHeaders_;

    /// Blocks containing a return (function exit points; mandatory boundaries).
    /// Together with the entry boundary, these bracket every callee so a call
    /// crosses a checkpoint on the way in and on the way out (PFI region model).
    llvm::SmallPtrSet<llvm::MachineBasicBlock *, 16> exitBlocks_;

    /// Blocks in reverse post-order
    std::vector<llvm::MachineBasicBlock *> topoOrder_;

    double regStoreEnergy_;
    MSP430Constants C_;
    llvm::DenseMap<const llvm::MachineBasicBlock *, llvm::SmallSet<llvm::MCPhysReg, 12>> liveIn_;
    llvm::SmallSet<llvm::MCPhysReg, 12> defsInRegion_;

    void identifyLoopHeaders();
    void identifyExitBlocks();
    void computeTopologicalOrder();
    double getBlockCost(llvm::MachineBasicBlock *MBB) const;

    void collectBlockDefs(llvm::MachineBasicBlock *MBB);
    double computeCkptOverhead(llvm::MachineBasicBlock *MBB) const;

    MachineRockClimbResult partitionRegions();
};

} // namespace checkpoint
