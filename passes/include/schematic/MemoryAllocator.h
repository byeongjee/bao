#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include <map>
#include <optional>
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
                                              const std::vector<llvm::BasicBlock *> &intervalBlocks,
                                              const SchematicStateAnalysis &state);

/// Estimate energy gain from placing a variable in VM.
/// Reference: memory_allocator.py:estimate_energy_gain (line 93).
double estimateEnergyGain(unsigned accessCount, unsigned varSizeBytes, bool needRestore,
                          bool needSave, const SchematicParams &params);

/// Compute optimal greedy allocation for an interval.
/// Reference: memory_allocator.py:choose_memory_allocation (line 142).
/// startConstraint/endConstraint: if set, variables that need restore/save
/// and exist in the constraint allocation are forced to NVM (reference lines 186-190).
/// accessScale: multiplier for variable access counts (used by convergence loop to
/// scale accesses by min(numIt, maxTripCount) iterations).
RegionAllocation
chooseMemoryAllocation(const std::vector<llvm::BasicBlock *> &intervalBlocks,
                       const SchematicStateAnalysis &state, const SchematicParams &params,
                       const std::map<llvm::Value *, Placement> &fixedPlacements,
                       VMAddressTracker *tracker, const RegionAllocation *startConstraint,
                       const RegionAllocation *endConstraint, unsigned accessScale);

/// Result of computeCost: allocation + net energy cost.
struct ComputeCostResult {
    RegionAllocation allocation;
    double energy;
};

/// Compute cost of an interval: execution energy minus VM placement gain.
/// Reference: memory_allocator.py:compute_cost (line 229).
ComputeCostResult computeCost(const std::vector<llvm::BasicBlock *> &blocks,
                              const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                              const SchematicParams &params,
                              const std::map<llvm::Value *, Placement> &fixedPlacements,
                              VMAddressTracker *tracker, const RegionAllocation *startConstraint,
                              const RegionAllocation *endConstraint);

/// Compute total energy gain from a memory allocation.
/// Reference: memory_allocator.py:compute_memory_allocation_gain.
double computeMemoryAllocationGain(const RegionAllocation &alloc,
                                   const std::vector<llvm::BasicBlock *> &blocks,
                                   const SchematicStateAnalysis &state,
                                   const SchematicParams &params);

/// Compute cost to restore VM-placed variables at a block.
/// Reference: memory_allocator.py:compute_allocation_restore_cost (line 68).
double computeAllocationRestoreCost(
    llvm::BasicBlock *BB,
    const llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>> &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params);

/// Compute cost to save VM-placed variables at a block.
/// Reference: memory_allocator.py:compute_allocation_save_cost (line 81).
double computeAllocationSaveCost(
    llvm::BasicBlock *BB,
    const llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>> &decidedPlacements,
    const SchematicStateAnalysis &state, const SchematicParams &params);

/// Mark selected checkpoints as enabled in the solution.
/// Reference: cfg_modification.py:update_checkpoint_type (line 156).
void updateCheckpointType(const std::vector<CFGEdge> &selectedCheckpoints,
                          SchematicSolution &solution);

/// Apply RCG result allocations to the solution and seed energy propagation.
/// Reference: schematic.py:apply_memory_allocation (line 384).
void applyMemoryAllocation(const RCGResult &result,
                           const std::vector<llvm::BasicBlock *> &pathBlocks,
                           llvm::BasicBlock *startBound, llvm::BasicBlock *endBound,
                           SchematicSolution &solution, const CFGAnalysis &cfg,
                           const SchematicStateAnalysis &state, const SchematicParams &params,
                           llvm::LoopInfo &LI, llvm::Loop *loopScope);

} // namespace checkpoint
