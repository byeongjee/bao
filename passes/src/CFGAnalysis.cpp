#include "CFGAnalysis.h"
#include "EnergyModel.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"

#include <cmath>

namespace checkpoint {

CFGAnalysis::CFGAnalysis(llvm::Function &F, llvm::LoopInfo &LI) {
    analyze(F, LI);
}

const BlockInfo &CFGAnalysis::getBlockInfo(const std::string &name) const {
    auto it = blockInfo_.find(name);
    if (it == blockInfo_.end()) {
        static BlockInfo empty{"", 0, 1.0, 0};
        return empty;
    }
    return it->second;
}

void CFGAnalysis::analyze(llvm::Function &F, llvm::LoopInfo &LI) {
    // Process each basic block
    for (llvm::BasicBlock &BB : F) {
        std::string name = BB.getName().str();

        // Handle unnamed blocks (entry block often has no name)
        if (name.empty()) {
            name = "bb" + std::to_string(blocks_.size());
        }

        blocks_.push_back(name);

        // Calculate energy cost
        int energyCost = 0;
        for (llvm::Instruction &I : BB) {
            energyCost += EnergyModel::getCost(I);
        }

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
        for (llvm::BasicBlock *Succ : llvm::successors(&BB)) {
            std::string succName = Succ->getName().str();
            if (succName.empty()) {
                // Find the index of the successor block
                size_t idx = 0;
                for (llvm::BasicBlock &B : F) {
                    if (&B == Succ) {
                        succName = "bb" + std::to_string(idx);
                        break;
                    }
                    idx++;
                }
            }
            edges_.emplace_back(name, succName);
        }
    }
}

} // namespace checkpoint
