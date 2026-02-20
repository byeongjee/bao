#include "common/CFGAnalysis.h"
#include "common/BlockUtils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"

namespace checkpoint {

CFGAnalysis::CFGAnalysis(llvm::Function &F, llvm::LoopInfo &LI,
                         EnergyEstimator &estimator) {
    analyze(F, LI, estimator);
}

const BlockInfo &CFGAnalysis::getBlockInfo(const llvm::BasicBlock *BB) const {
    auto it = blockInfo_.find(BB);
    if (it == blockInfo_.end()) {
        static BlockInfo empty{"", 0.0};
        return empty;
    }
    return it->second;
}

void CFGAnalysis::analyze(llvm::Function &F, llvm::LoopInfo &LI,
                          EnergyEstimator &estimator) {
    // Process each basic block
    for (llvm::BasicBlock &BB : F) {
        std::string name = getBlockName(BB, F);

        blocks_.push_back(&BB);

        // Calculate energy cost using estimator
        EnergyEstimate estimate = estimator.estimate(BB);
        double energyCost = estimate.cost;

        BlockInfo info{name, energyCost};
        blockInfo_[&BB] = info;

        // Set entry block
        if (&BB == &F.getEntryBlock()) {
            entryBlock_ = &BB;
        }

        // Add edges to successors
        bool hasSuccessors = false;
        for (llvm::BasicBlock *Succ : llvm::successors(&BB)) {
            hasSuccessors = true;
            edges_.emplace_back(&BB, Succ);
        }

        // Track exit blocks (no successors)
        if (!hasSuccessors) {
            exitBlocks_.push_back(&BB);
        }
    }
}

} // namespace checkpoint
