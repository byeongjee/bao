#pragma once

#include "estimator/EnergyEstimator.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"

#include <string>
#include <vector>

namespace checkpoint {

/// Information about a basic block in the CFG.
struct BlockInfo {
    std::string name;       // Display name for debug/logging
    double energyCost;      // Energy cost from estimator
};

/// CFG analysis using LLVM's infrastructure.
/// Extracts block information, edges, and loop depths.
class CFGAnalysis {
public:
    /// Construct CFG analysis for a function with an energy estimator.
    /// @param F The function to analyze.
    /// @param LI Loop information from LLVM's LoopAnalysis.
    /// @param estimator Energy estimator to compute block costs.
    CFGAnalysis(llvm::Function &F, llvm::LoopInfo &LI, EnergyEstimator &estimator);

    /// Get all blocks in function order.
    const std::vector<const llvm::BasicBlock *> &getBlocks() const {
        return blocks_;
    }

    /// Get information about a specific block.
    const BlockInfo &getBlockInfo(const llvm::BasicBlock *BB) const;

    /// Get all edges as (source, destination) pairs.
    const std::vector<std::pair<const llvm::BasicBlock *,
                                const llvm::BasicBlock *>> &
    getEdges() const {
        return edges_;
    }

    /// Get the entry block.
    const llvm::BasicBlock *getEntryBlock() const { return entryBlock_; }

    /// Get exit blocks (blocks with no successors).
    const std::vector<const llvm::BasicBlock *> &getExitBlocks() const {
        return exitBlocks_;
    }

private:
    std::vector<const llvm::BasicBlock *> blocks_;
    llvm::DenseMap<const llvm::BasicBlock *, BlockInfo> blockInfo_;
    std::vector<std::pair<const llvm::BasicBlock *,
                          const llvm::BasicBlock *>> edges_;
    const llvm::BasicBlock *entryBlock_ = nullptr;
    std::vector<const llvm::BasicBlock *> exitBlocks_;

    void analyze(llvm::Function &F, llvm::LoopInfo &LI, EnergyEstimator &estimator);
};

} // namespace checkpoint
