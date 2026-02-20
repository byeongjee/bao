#pragma once

#include "rockclimb/RockClimbOptimizer.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

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
    std::string regionName;        // Name of the region this belongs to
};

/// Distributed checkpointing analysis.
/// Implements RockClimb's register checkpointing strategy:
/// Save registers at their last definition point in each region.
/// This is in contrast to boundary checkpointing which saves at region starts.
class DistributedCheckpointing {
public:
    /// Construct analyzer for a function and its regions.
    /// @param F The function being analyzed.
    /// @param regions Region information from RockClimbOptimizer.
    DistributedCheckpointing(llvm::Function &F,
                             const std::vector<RegionInfo> &regions);

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
    llvm::Function &F_;
    const std::vector<RegionInfo> &regions_;
    unsigned nextRegId_ = 0;

    /// Map from block name to BasicBlock*.
    std::map<std::string, llvm::BasicBlock*> blockMap_;

    /// Build block name to pointer mapping.
    void buildBlockMap();

    /// Compute registers defined in a region.
    std::set<llvm::Value*> computeDefs(const RegionInfo &region);

    /// Compute registers live at region exit.
    std::set<llvm::Value*> computeLiveOut(const RegionInfo &region);

    /// Find the last definition point of a register within a region.
    /// @return The instruction defining the register, or nullptr if not in region.
    llvm::Instruction* findLastDef(llvm::Value *reg, const RegionInfo &region);

    /// Assign a unique register ID for NVM storage.
    unsigned assignRegId(llvm::Value *reg);

    /// Map from Value* to assigned register ID.
    std::map<llvm::Value*, unsigned> regIdMap_;
};

} // namespace checkpoint
