#pragma once

#include "rockclimb/RockClimbOptimizer.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Information about a single register checkpoint point.
struct CheckpointPoint {
    llvm::Instruction *afterInst;  // Insert checkpoint store after this instruction
    llvm::Value *reg;              // The register (SSA value) to save
    unsigned regId;                // Register ID for NVM storage offset
    llvm::BasicBlock *regionStart; // Start block of the region this belongs to
};

/// Distributed checkpointing analysis.
/// Implements RockClimb's register checkpointing strategy:
/// Save registers at their last definition point in each region.
/// This is in contrast to boundary checkpointing which saves at region starts.
class DistributedCheckpointing {
  public:
    /// Construct analyzer for regions.
    /// @param regions Region information from RockClimbOptimizer.
    explicit DistributedCheckpointing(const std::vector<RegionInfo> &regions);

    /// Analyze and compute checkpoint points for distributed checkpointing.
    /// For each region r:
    ///   Def_r = registers defined in r
    ///   LiveOut_r = registers live at region exit
    ///   ckpt_r = Def_r ∩ LiveOut_r
    /// @return Vector of checkpoint points with instruction and register info.
    std::vector<CheckpointPoint> analyze();

    /// Get the number of registers that need checkpointing.
    unsigned getCheckpointedRegisterCount() const { return nextRegId_; }

  private:
    const std::vector<RegionInfo> &regions_;
    unsigned nextRegId_ = 0;

    /// Build a set of BasicBlock* from a region's WeakTrackingVH block list.
    static llvm::SmallPtrSet<llvm::BasicBlock *, 8> makeRegionBlockSet(const RegionInfo &region);

    /// Compute registers defined in a region.
    std::set<llvm::Value *> computeDefs(const RegionInfo &region);

    /// Compute registers live at region exit.
    std::set<llvm::Value *> computeLiveOut(const RegionInfo &region);

    /// Assign a unique register ID for NVM storage.
    unsigned assignRegId(llvm::Value *reg);

    /// Map from Value* to assigned register ID.
    std::map<llvm::Value *, unsigned> regIdMap_;
};

} // namespace checkpoint
