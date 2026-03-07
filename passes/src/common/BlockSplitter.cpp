#include "common/BlockSplitter.h"

#include "llvm/IR/Instructions.h"

namespace checkpoint {

llvm::BasicBlock *splitOversizedBlock(llvm::BasicBlock *BB, double threshold,
                                      EnergyEstimator &estimator) {
    if (!BB)
        return nullptr;

    // Find the split point: accumulate per-instruction energy until threshold
    double accumulated = 0.0;
    llvm::Instruction *splitPoint = nullptr;
    llvm::Instruction *lastNonTerm = nullptr;

    for (llvm::Instruction &I : *BB) {
        // Don't split before a PHI node or landingpad
        if (llvm::isa<llvm::PHINode>(&I) || llvm::isa<llvm::LandingPadInst>(&I))
            continue;

        double instCost = estimator.getInstructionCost(I);

        if (accumulated + instCost >= threshold && splitPoint) {
            // Split before this instruction
            break;
        }

        accumulated += instCost;
        splitPoint = &I;

        // Don't set split point to the terminator
        if (I.isTerminator()) {
            splitPoint = nullptr;
        } else {
            lastNonTerm = &I;
        }
    }

    // Fallback: if threshold crossing was not found (e.g., model granularity or
    // rounding), still try a structural split at the last non-terminator.
    if (!splitPoint)
        splitPoint = lastNonTerm;

    if (!splitPoint)
        return nullptr; // Can't split (block too small or all PHIs)

    // Split after splitPoint — the next instruction becomes the start of the new block
    llvm::Instruction *splitBefore = splitPoint->getNextNode();
    if (!splitBefore || splitBefore->isTerminator()) {
        if (splitPoint->isTerminator())
            return nullptr;
        splitBefore = splitPoint;
    }

    // Perform the split
    std::string blockName = BB->hasName() ? BB->getName().str() : "bb";
    llvm::BasicBlock *newBB = BB->splitBasicBlock(splitBefore, blockName + ".split");
    return newBB;
}

bool splitAllOversizedBlocks(llvm::Function &F, double threshold, EnergyEstimator &estimator,
                             llvm::LoopInfo &LI, CFGAnalysis &cfg) {
    bool changed = true;
    unsigned maxIterations = 1000; // Safety bound

    while (changed && maxIterations-- > 0) {
        changed = false;
        for (llvm::BasicBlock &BB : F) {
            double blockEnergy = estimator.estimate(BB).cost;
            if (blockEnergy > threshold) {
                llvm::BasicBlock *newBB = splitOversizedBlock(&BB, threshold, estimator);
                if (!newBB) {
                    // Unsplittable block exceeds capacity
                    llvm::errs() << "Unsplittable block '" << BB.getName() << "' in function '"
                                 << F.getName()
                                 << "' exceeds split threshold (energy=" << blockEnergy
                                 << ", threshold=" << threshold << ")\n";
                    double maxInstCost = 0.0;
                    const llvm::Instruction *maxInst = nullptr;
                    for (const llvm::Instruction &I : BB) {
                        if (llvm::isa<llvm::PHINode>(I) || llvm::isa<llvm::LandingPadInst>(I))
                            continue;
                        double cost = estimator.getInstructionCost(I);
                        if (cost > maxInstCost) {
                            maxInstCost = cost;
                            maxInst = &I;
                        }
                    }
                    if (maxInst) {
                        llvm::errs() << "  Max instruction cost: " << maxInstCost << " | inst: ";
                        maxInst->print(llvm::errs());
                        llvm::errs() << "\n";
                    }
                    return false;
                }
                changed = true;
                break; // Restart iteration since BB list changed
            }
        }
    }

    // Rebuild CFGAnalysis after all splits
    cfg = CFGAnalysis(F, LI, estimator);
    return true;
}

} // namespace checkpoint
