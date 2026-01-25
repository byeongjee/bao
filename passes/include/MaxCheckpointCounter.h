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
    std::vector<llvm::BasicBlock*> criticalPath;     ///< Path with max checkpoints
};

/// Counts the maximum number of checkpoints on any execution path.
/// Uses dynamic programming with bounded loop iterations.
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
    /// @return Result containing max count and critical path.
    CountResult compute();

private:
    llvm::Function &F_;
    llvm::LoopInfo &LI_;
    std::set<llvm::BasicBlock*> checkpoints_;
    unsigned defaultBound_ = 2;
    std::map<const llvm::Loop*, unsigned> loopBounds_;

    /// Back-edges detected in the CFG (latch -> header).
    std::vector<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>> backEdges_;

    /// Index of each back-edge for the iteration count vector.
    std::map<std::pair<llvm::BasicBlock*, llvm::BasicBlock*>, int> backEdgeIndex_;

    /// Memoization cache: (BasicBlock*, edgeCounts) -> max checkpoints from that state.
    std::map<std::pair<llvm::BasicBlock*, std::vector<int>>, int> memo_;

    /// Loops that have already been warned about using the default bound.
    mutable std::set<const llvm::Loop*> warnedLoops_;

    /// Find all back-edges in the CFG using DFS.
    void findBackEdges();

    /// Check if an edge is a back-edge.
    bool isBackEdge(llvm::BasicBlock *From, llvm::BasicBlock *To) const;

    /// Get the trip count bound for a loop.
    unsigned getLoopBound(llvm::Loop *L) const;

    /// Recursive DP function to count max checkpoints.
    /// @param BB Current basic block.
    /// @param edgeCounts Current iteration counts for each back-edge.
    /// @param path Current path (for tracking critical path).
    /// @return Maximum checkpoints from BB to any exit.
    int countFromBlock(llvm::BasicBlock *BB,
                       std::vector<int> &edgeCounts,
                       std::vector<llvm::BasicBlock*> &path);
};

} // namespace checkpoint
