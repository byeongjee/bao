#pragma once

#include "rockclimb/RockClimbOptimizer.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
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

/// Checkpoint point for a memory location (alloca or global).
struct MemoryCheckpointPoint {
    llvm::Value *memLoc;          // AllocaInst* or GlobalVariable*
    llvm::Type *valueType;        // Type of the stored value
    std::string nvmSlotName;      // Name of NVM global to store to
    unsigned boundaryId;          // Which boundary this belongs to
    std::string regionName;       // Region name for debugging
};

/// Result of memory checkpoint analysis.
struct MemoryCheckpointResult {
    std::vector<MemoryCheckpointPoint> checkpoints;

    // Map: boundaryId -> list of memory locations to save
    std::map<unsigned, std::vector<MemoryCheckpointPoint>> byBoundary;
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
    /// @param boundaries Region boundary block names (for memory checkpointing).
    DistributedCheckpointing(llvm::Function &F,
                             const std::vector<RegionInfo> &regions,
                             const std::vector<std::string> &boundaries = {});

    /// Analyze and compute checkpoint points for distributed checkpointing.
    /// For each region r:
    ///   Def_r = registers defined in r
    ///   LiveOut_r = registers live at region exit
    ///   ckpt_r = Def_r ∩ LiveOut_r
    /// @return Vector of checkpoint points with instruction and register info.
    std::vector<CheckpointPoint> analyze();

    /// Analyze memory locations (allocas and globals) that need checkpointing.
    /// For each region boundary, finds memory locations that are:
    /// - Modified within the region (written to)
    /// - Read after the boundary (live-out)
    /// @return MemoryCheckpointResult with checkpoints organized by boundary.
    MemoryCheckpointResult analyzeMemory();

    /// Get the number of registers that need checkpointing.
    unsigned getCheckpointedRegisterCount() const { return nextRegId_; }

private:
    llvm::Function &F_;
    const std::vector<RegionInfo> &regions_;
    const std::vector<std::string> boundaries_;
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

    // === Memory checkpointing analysis ===

    /// Find allocas that are live at region boundary.
    /// An alloca is live-out if it is stored to in the region and
    /// loaded from after the region.
    std::set<llvm::AllocaInst*> computeLiveAllocas(const RegionInfo &region);

    /// Find globals that are live at region boundary.
    /// A global is live-out if it is stored to in the region and
    /// loaded from after the region.
    std::set<llvm::GlobalVariable*> computeLiveGlobals(const RegionInfo &region);

    /// Check if an alloca is modified within the region.
    bool isModifiedInRegion(llvm::AllocaInst *alloca, const RegionInfo &region);

    /// Check if a global is modified within the region.
    bool isModifiedInRegion(llvm::GlobalVariable *global, const RegionInfo &region);

    /// Check if a memory location (alloca or global) is read after the region.
    bool isReadAfterRegion(llvm::Value *memLoc, const RegionInfo &region);

    /// Generate NVM slot name for a memory checkpoint.
    std::string generateNVMSlotName(unsigned boundaryId, llvm::Value *memLoc);
};

} // namespace checkpoint
