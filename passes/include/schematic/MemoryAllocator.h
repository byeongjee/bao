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

/// Compute total interval energy (spec §7.2).
double computeIntervalEnergy(const std::vector<llvm::BasicBlock *> &intervalBlocks,
                             const RegionAllocation &allocation,
                             const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                             const SchematicParams &params, bool isFirstInterval,
                             bool isLastInterval);

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

} // namespace checkpoint
