#include "common/CFGAnalysis.h"
#include "common/BlockUtils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"

#include <cmath>

namespace checkpoint {

CFGAnalysis::CFGAnalysis(llvm::Function &F, llvm::LoopInfo &LI,
                         EnergyEstimator &estimator) {
    analyze(F, LI, estimator);
}

const BlockInfo &CFGAnalysis::getBlockInfo(const std::string &name) const {
    auto it = blockInfo_.find(name);
    if (it == blockInfo_.end()) {
        static BlockInfo empty{"", 0.0, 1.0, 0};
        return empty;
    }
    return it->second;
}

void CFGAnalysis::analyze(llvm::Function &F, llvm::LoopInfo &LI,
                          EnergyEstimator &estimator) {
    // Process each basic block
    for (llvm::BasicBlock &BB : F) {
        std::string name = getBlockName(BB, F);

        blocks_.push_back(name);

        // Calculate energy cost using estimator
        EnergyEstimate estimate = estimator.estimate(BB);
        double energyCost = estimate.cost;

        // Get loop depth
        int loopDepth = LI.getLoopDepth(&BB);

        // Estimate frequency based on loop depth
        // freq = 10^loopDepth (heuristic)
        double freq = std::pow(10.0, static_cast<double>(loopDepth));

        BlockInfo info{name, energyCost, freq, loopDepth};
        blockInfo_[name] = info;

        // Set entry block
        if (&BB == &F.getEntryBlock()) {
            entryBlock_ = name;
        }

        // Add edges to successors
        bool hasSuccessors = false;
        for (llvm::BasicBlock *Succ : llvm::successors(&BB)) {
            hasSuccessors = true;
            std::string succName = getBlockName(Succ, F);
            edges_.emplace_back(name, succName);
        }

        // Track exit blocks (no successors)
        if (!hasSuccessors) {
            exitBlocks_.push_back(name);
        }
    }
}

} // namespace checkpoint
