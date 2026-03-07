#pragma once

#include "common/CFGAnalysis.h"
#include "estimator/EnergyEstimator.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"

#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Information about a region formed by RockClimb partitioning.
struct RegionInfo {
    llvm::WeakTrackingVH startBlock;          // First block of the region
    std::vector<llvm::WeakTrackingVH> blocks; // All blocks in the region
    std::set<llvm::Value *> liveOutRegisters; // Registers live at region exit
    double totalEnergy;                       // Total energy of the region
};

/// RockClimb optimizer: greedy region partitioning.
/// Partitions CFG into regions where WCET(region) < E_safe.
/// Implements Algorithm 1 from the RockClimb paper with path-aware
/// energy accumulation using per-block IncomeCycle propagation.
class RockClimbOptimizer {
  public:
    /// Result of optimization.
    struct Result {
        std::vector<llvm::WeakTrackingVH> regionBoundaries; // Blocks starting new regions
        std::vector<RegionInfo> regions;                    // Detailed region information
        bool feasible;                                      // True if partitioning succeeded
        std::string errorMessage;                           // Error description if not feasible
    };

    /// Construct optimizer for a CFG.
    /// @param cfg The CFG analysis results.
    /// @param E_safe Maximum safe energy per region.
    /// @param LI Loop info for mandatory header boundaries.
    /// @param F The function being optimized.
    /// @param estimator Energy estimator for per-instruction costs (for block splitting).
    RockClimbOptimizer(const CFGAnalysis &cfg, double E_safe, llvm::LoopInfo &LI, llvm::Function &F,
                       EnergyEstimator *estimator = nullptr);

    /// Run the optimization algorithm.
    /// @return Result containing region boundaries and info.
    Result optimize();

    /// Get blocks that exceed E_safe individually (infeasible).
    std::vector<llvm::BasicBlock *> getInfeasibleBlocks() const;

    /// Add extra block costs from checkpoint stores (CkptCycles_bbi).
    /// Additive: each call accumulates into energyCosts_. Call at most once
    /// per optimization run. Applied before Algorithm 1 traversal so
    /// effective per-block cost is Cycle_ori + CkptCycles.
    /// @param costs Map from block pointer to additional energy cost.
    void setExtraBlockCosts(const llvm::DenseMap<llvm::BasicBlock *, double> &costs);

    /// Resolve a WeakTrackingVH to a BasicBlock pointer.
    /// Asserts if the handle has been invalidated (block deleted).
    static llvm::BasicBlock *resolveBlock(const llvm::WeakTrackingVH &handle);

  private:
    const CFGAnalysis &cfg_;
    double E_safe_;
    llvm::LoopInfo &LI_;
    llvm::Function &F_;
    EnergyEstimator *estimator_;

    /// Topologically sorted blocks.
    std::vector<llvm::WeakTrackingVH> topoOrder_;

    /// Loop headers (mandatory region boundaries).
    llvm::SmallPtrSet<llvm::BasicBlock *, 8> loopHeaders_;

    /// Blocks containing function calls (mandatory region boundaries).
    llvm::SmallPtrSet<llvm::BasicBlock *, 8> callSiteBlocks_;

    /// Per-block energy costs (replaces CFGAnalysis lookups + extraBlockCosts_).
    llvm::DenseMap<llvm::BasicBlock *, double> energyCosts_;

    /// Build topological order of blocks.
    void computeTopologicalOrder();

    /// Identify loop headers from LoopInfo.
    void identifyLoopHeaders();

    /// Identify blocks containing function calls to non-intrinsic functions.
    void identifyCallSiteBlocks();

    /// Get effective block cost.
    double getBlockCost(llvm::BasicBlock *BB) const;

    /// Main partitioning algorithm (Algorithm 1 from RockClimb paper).
    Result partitionRegions();

    /// Split a block when its cost exceeds threshold (Algorithm 1 while loop).
    /// Splits the LLVM BasicBlock at the instruction where accumulated cost
    /// reaches the threshold, updates local data structures.
    /// @param BB The block to split.
    /// @param threshold Energy threshold (E_safe).
    /// @param insertIdx Index in topoOrder_ after which the new block is inserted.
    /// @return Pointer to the new (second half) block, or nullptr if split not possible.
    llvm::BasicBlock *splitBlock(llvm::BasicBlock *BB, double threshold, size_t insertIdx);
};

} // namespace checkpoint
