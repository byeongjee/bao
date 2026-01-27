#pragma once

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <map>
#include <set>
#include <vector>

namespace checkpoint {

/// Result of the maximum checkpoint counting analysis.
struct CountResult {
    int maxCheckpoints;                              ///< Max checkpoints on any path
    std::vector<llvm::BasicBlock*> criticalPath;     ///< Blocks with checkpoints
};

/// Counts the maximum number of checkpoints on any execution path.
///
/// Algorithm: Closed-form computation
/// ----------------------------------
/// For each checkpoint block B, compute how many times it can execute:
///   multiplier = product of bounds for all loops containing B
/// Sum across all checkpoints to get the maximum.
///
/// Complexity: O(checkpoints × max_loop_depth) ≈ linear
///
/// Trade-off:
/// ----------
/// This closed-form approach may OVER-COUNT if checkpoints are in mutually
/// exclusive branches (e.g., if-then-else where only one branch executes).
/// In such cases, the result is a conservative UPPER BOUND, not the exact max.
///
/// This is acceptable for worst-case analysis (e.g., estimating max checkpoint
/// overhead) but would be incorrect if exact path counting is required.
///
/// The previous path-enumeration algorithm was exact but had exponential
/// complexity O(bound₁ × bound₂ × ... × boundₙ), making it infeasible for
/// loops with high iteration counts (e.g., 128 × 128 × 8 = 131K states/block).
///
class MaxCheckpointCounter {
public:
    /// Construct a counter for the given function.
    /// @param F The function to analyze.
    /// @param LI Loop information for the function.
    /// @param checkpoints Set of basic blocks that have checkpoints.
    MaxCheckpointCounter(llvm::Function &F,
                         llvm::LoopInfo &LI,
                         const std::set<llvm::BasicBlock*> &checkpoints);

    /// Set trip counts for loops.
    /// @param bounds Map from Loop* to trip count (from LoopTripCount::extractBounds).
    void setLoopBounds(const std::map<const llvm::Loop*, unsigned> &bounds);

    /// Set the default bound for loops without annotations.
    /// @param bound Default trip count (default: 2).
    void setDefaultBound(unsigned bound) { defaultBound_ = bound; }

    /// Count maximum checkpoints on any path from entry to exit.
    /// @return Result containing max count and list of checkpoint blocks.
    CountResult compute();

private:
    llvm::Function &F_;
    llvm::LoopInfo &LI_;
    std::set<llvm::BasicBlock*> checkpoints_;
    unsigned defaultBound_ = 2;
    std::map<const llvm::Loop*, unsigned> loopBounds_;

    /// Loops that have already been warned about using the default bound.
    mutable std::set<const llvm::Loop*> warnedLoops_;

    /// Get the trip count bound for a loop.
    unsigned getLoopBound(llvm::Loop *L) const;
};

} // namespace checkpoint
