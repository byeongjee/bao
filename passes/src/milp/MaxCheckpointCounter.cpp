#include "milp/MaxCheckpointCounter.h"

#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

namespace checkpoint {

MaxCheckpointCounter::MaxCheckpointCounter(
    llvm::Function &F,
    llvm::LoopInfo &LI,
    const std::set<llvm::BasicBlock*> &checkpoints)
    : F_(F), LI_(LI), checkpoints_(checkpoints) {}

void MaxCheckpointCounter::setLoopBounds(
    const std::map<const llvm::Loop*, unsigned> &bounds) {
    loopBounds_ = bounds;
}

unsigned MaxCheckpointCounter::getLoopBound(llvm::Loop *L) const {
    auto it = loopBounds_.find(L);
    if (it != loopBounds_.end()) {
        return it->second;
    }
    // Warn once per loop about using the default bound
    if (L && warnedLoops_.insert(L).second) {
        llvm::BasicBlock *Header = L->getHeader();
        llvm::errs() << "Warning: Loop";
        if (Header && Header->hasName()) {
            llvm::errs() << " @ " << Header->getName();
        }
        llvm::errs() << " has no __loop_tripcount annotation, using default bound ("
                     << defaultBound_ << ")\n";
    }
    return defaultBound_;
}

CountResult MaxCheckpointCounter::compute() {
    // Closed-form algorithm:
    // For each checkpoint block, compute how many times it can execute on the
    // worst-case path. This equals the product of all loop bounds for loops
    // containing that block.
    //
    // Example: A checkpoint in a doubly-nested loop with bounds (100, 50)
    // can execute up to 100 * 50 = 5000 times.
    //
    // Trade-off: This may over-count if checkpoints are in mutually exclusive
    // branches (if-then-else). The result is a conservative upper bound.
    // See header comment for detailed discussion.

    int maxCount = 0;
    std::vector<llvm::BasicBlock*> checkpointBlocks;

    for (llvm::BasicBlock *BB : checkpoints_) {
        checkpointBlocks.push_back(BB);

        // Compute multiplier: product of bounds for all containing loops
        unsigned multiplier = 1;
        llvm::Loop *L = LI_.getLoopFor(BB);
        while (L) {
            multiplier *= getLoopBound(L);
            L = L->getParentLoop();
        }

        maxCount += multiplier;
    }

    // Note: criticalPath now contains all checkpoint blocks rather than
    // a specific execution path. This is sufficient for reporting purposes
    // and avoids the complexity of path reconstruction.
    return CountResult{maxCount, std::move(checkpointBlocks)};
}

} // namespace checkpoint
