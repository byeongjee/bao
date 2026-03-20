#include "common/EdgeSplitPass.h"
#include "common/Logger.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <vector>

using namespace llvm;

namespace checkpoint {

PreservedAnalyses EdgeSplitPass::run(Function &F, FunctionAnalysisManager &AM) {
    initLogging();

    if (F.isDeclaration())
        return PreservedAnalyses::all();

    // Collect edges to split. We must not mutate the CFG while iterating.
    std::vector<std::pair<BasicBlock *, BasicBlock *>> edgesToSplit;
    for (BasicBlock &BB : F) {
        SmallVector<BasicBlock *, 4> preds(predecessors(&BB));
        if (preds.size() <= 1)
            continue;
        // Skip EH pads — SplitEdge cannot split edges into landing pads.
        if (BB.isEHPad())
            continue;
        for (BasicBlock *pred : preds) {
            edgesToSplit.emplace_back(pred, &BB);
        }
    }

    if (edgesToSplit.empty())
        return PreservedAnalyses::all();

    PLOGI << "EdgeSplit: splitting " << edgesToSplit.size() << " edges in " << F.getName();

    for (auto &[pred, succ] : edgesToSplit) {
        SplitEdge(pred, succ);
    }

    // Verify: every immediate predecessor of a merge point has exactly
    // one predecessor (i.e., is a fresh split block).
    for (BasicBlock &BB : F) {
        if (BB.isEHPad())
            continue;
        SmallVector<BasicBlock *, 4> preds(predecessors(&BB));
        if (preds.size() <= 1)
            continue;
        for (BasicBlock *pred : preds) {
            unsigned predPredCount = 0;
            for ([[maybe_unused]] BasicBlock *pp : predecessors(pred))
                ++predPredCount;
            assert(predPredCount == 1 && "EdgeSplitPass: predecessor of merge point has != 1 "
                                         "predecessor after splitting");
        }
    }

    PLOGI << "EdgeSplit: done, all merge-point predecessors have single "
             "predecessor";

    // CFG changed — invalidate all analyses.
    return PreservedAnalyses::none();
}

} // namespace checkpoint
