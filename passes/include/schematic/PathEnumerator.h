#pragma once

#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <set>
#include <vector>

namespace checkpoint {

struct EnumeratedPath {
    std::vector<llvm::BasicBlock *> blocks;
    double probability;
    unsigned count = 0; // execution count from trace (0 = BPI-enumerated)
};

class PathEnumerator {
public:
    PathEnumerator(llvm::Function &F,
                   llvm::BranchProbabilityInfo &BPI,
                   llvm::LoopInfo &LI,
                   unsigned maxPaths);

    /// Enumerate paths in decreasing frequency. Returns sorted vector.
    std::vector<EnumeratedPath> enumerate();

private:
    llvm::Function &F_;
    llvm::BranchProbabilityInfo &BPI_;
    llvm::LoopInfo &LI_;
    unsigned maxPaths_;

    std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>>
    identifyBackEdges() const;

    std::vector<EnumeratedPath> generateCoveragePaths(
        const std::set<llvm::BasicBlock *> &uncovered,
        const std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> &backEdges) const;
};

} // namespace checkpoint
