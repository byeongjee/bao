#pragma once

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"

#include <map>
#include <string>
#include <vector>

namespace checkpoint {

/// Information about a basic block in the CFG.
struct BlockInfo {
    std::string name;
    int energyCost;    // Sum of instruction costs in the block
    double freq;       // Estimated execution frequency
    int loopDepth;     // Nesting depth (0 = not in loop)
};

/// CFG analysis using LLVM's infrastructure.
/// Extracts block information, edges, and loop depths.
class CFGAnalysis {
public:
    /// Construct CFG analysis for a function.
    /// @param F The function to analyze.
    /// @param LI Loop information from LLVM's LoopAnalysis.
    CFGAnalysis(llvm::Function &F, llvm::LoopInfo &LI);

    /// Get all block names in order.
    const std::vector<std::string> &getBlocks() const { return blocks_; }

    /// Get information about a specific block.
    const BlockInfo &getBlockInfo(const std::string &name) const;

    /// Get all edges as (source, destination) pairs.
    const std::vector<std::pair<std::string, std::string>> &getEdges() const {
        return edges_;
    }

    /// Get the entry block name.
    const std::string &getEntryBlock() const { return entryBlock_; }

    /// Get exit blocks (blocks with no successors).
    const std::vector<std::string> &getExitBlocks() const { return exitBlocks_; }

private:
    std::vector<std::string> blocks_;
    std::map<std::string, BlockInfo> blockInfo_;
    std::vector<std::pair<std::string, std::string>> edges_;
    std::string entryBlock_;
    std::vector<std::string> exitBlocks_;

    void analyze(llvm::Function &F, llvm::LoopInfo &LI);
};

} // namespace checkpoint
