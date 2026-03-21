#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace checkpoint {

struct RCGResult; // Forward declaration for applyMemoryAllocation

/// Tracks VM address assignments across intervals within a function.
/// Variables that were previously allocated in VM reuse their address.
/// Matches reference: `already_allocated_variables` + `top_address`.
class VMAddressTracker {
  public:
    void reset();
    std::optional<unsigned> getExistingAddress(llvm::Value *v) const;
    unsigned recordAllocation(llvm::Value *v, unsigned size);
    unsigned getTopAddress() const;

  private:
    std::map<llvm::Value *, unsigned> allocatedVars_;
    unsigned topAddress_ = 0;
};

/// Compute needRestore and needSave flags for variable v in an interval.
/// needRestore: true if first access to v is a read (needs restore at interval start)
/// needSave: always true (reference SCHEMATIC algorithm unconditionally saves)
std::pair<bool, bool> computeSaveRestoreFlags(llvm::Value *v,
                                              const std::vector<SchematicBlock *> &intervalBlocks,
                                              const SchematicStateAnalysis &state);

/// Estimate energy gain from placing a variable in VM.
/// Reference: memory_allocator.py:estimate_energy_gain (line 93).
double estimateEnergyGain(unsigned accessCount, unsigned varSizeBytes, bool needRestore,
                          bool needSave, const SchematicParams &params);

/// Merge multiple memory allocations into one, with conflict detection.
/// Reference: memory_allocation.py:merge_allocations (line 217).
/// Returns nullopt on incompatible allocations (equivalent to IncompatibleMemAllocException).
std::optional<RegionAllocation>
mergeAllocations(const std::vector<const RegionAllocation *> &allocations,
                 const SchematicStateAnalysis &state, bool checkpointIncreaseAllowed);

/// Compute optimal greedy allocation for an interval.
/// Reference: memory_allocator.py:choose_memory_allocation (line 124).
/// Merges memoryAllocations + startAlloc/endAlloc internally;
/// returns (MemoryAllocation(), -99999) on incompatible allocations.
/// accessScale: multiplier for variable access counts (convergence loop).
std::pair<RegionAllocation, double>
chooseMemoryAllocation(const std::vector<SchematicBlock *> &intervalBlocks,
                       const SchematicStateAnalysis &state, const SchematicParams &params,
                       const RegionAllocation *startAlloc, const RegionAllocation *endAlloc,
                       const std::vector<const RegionAllocation *> &memoryAllocations,
                       VMAddressTracker *tracker, unsigned accessScale);

/// Result of computeCost: allocation + net energy cost.
struct ComputeCostResult {
    RegionAllocation allocation;
    double energy;
};

/// Compute cost of an interval: execution energy minus VM placement gain.
/// Reference: memory_allocator.py:compute_cost (line 229).
ComputeCostResult computeCost(
    const std::vector<SchematicBlock *> &blocks, const SchematicStateAnalysis &state,
    const CFGAnalysis &cfg, const SchematicParams &params,
    const std::unordered_map<SchematicBlock *, std::shared_ptr<RegionAllocation>> &blockAllocation,
    VMAddressTracker *tracker, const RegionAllocation *startAlloc,
    const RegionAllocation *endAlloc);

/// Compute cost to restore VM-placed variables at a block.
/// Reference: memory_allocator.py:compute_allocation_restore_cost (line 68).
double computeAllocationRestoreCost(
    SchematicBlock *block,
    const std::unordered_map<SchematicBlock *, std::map<llvm::Value *, Placement>>
        &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params);

/// Compute cost to save VM-placed variables at a block.
/// Reference: memory_allocator.py:compute_allocation_save_cost (line 81).
double computeAllocationSaveCost(
    SchematicBlock *block,
    const std::unordered_map<SchematicBlock *, std::map<llvm::Value *, Placement>>
        &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params);

/// Mark selected checkpoints as enabled in the solution.
/// Reference: cfg_modification.py:update_checkpoint_type (line 156).
void updateCheckpointType(const std::vector<CFGEdge> &selectedCheckpoints,
                          SchematicSolution &solution);

/// Apply RCG result allocations to the solution and seed energy propagation.
/// Reference: schematic.py:apply_memory_allocation (line 384).
void applyMemoryAllocation(const RCGResult &result, const std::vector<SchematicBlock *> &trace,
                           SchematicSolution &solution, const CFGAnalysis &cfg,
                           const SchematicStateAnalysis &state, const SchematicParams &params,
                           llvm::LoopInfo &LI, llvm::Loop *loopScope);

} // namespace checkpoint
