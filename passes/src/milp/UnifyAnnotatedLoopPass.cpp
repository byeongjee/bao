#include "milp/UnifyAnnotatedLoopPass.h"

#include "common/Logger.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

namespace checkpoint {

using namespace llvm;

namespace {

Loop *findSplitInnerLoop(Loop *Outer) {
    MDNode *LoopID = Outer->getLoopID();
    BasicBlock *OuterLatch = Outer->getLoopLatch();
    if (!LoopID || !Outer->getLoopPreheader() || !OuterLatch)
        return nullptr;

    BasicBlock *OuterHeader = Outer->getHeader();
    auto *HeaderBranch = dyn_cast<UncondBrInst>(OuterHeader->getTerminator());
    if (!HeaderBranch)
        return nullptr;

    for (Instruction &I : *OuterHeader) {
        if (!isa<PHINode>(I) && !I.isTerminator() && !isSafeToSpeculativelyExecute(&I))
            return nullptr;
    }

    BasicBlock *InnerHeader = HeaderBranch->getSuccessor(0);
    if (isa<PHINode>(InnerHeader->front()))
        return nullptr;

    for (Loop *Inner : Outer->getSubLoops()) {
        if (Inner->getHeader() != InnerHeader || Inner->getLoopID() != LoopID)
            continue;
        BasicBlock *InnerLatch = Inner->getLoopLatch();
        if (!InnerLatch)
            return nullptr;
        for (BasicBlock *Pred : predecessors(InnerHeader)) {
            if (Pred != OuterHeader && Pred != InnerLatch)
                return nullptr;
        }
        return Inner;
    }
    return nullptr;
}

void unifyPair(Loop *Outer, Loop *Inner) {
    BasicBlock *OuterHeader = Outer->getHeader();
    BasicBlock *OuterLatch = Outer->getLoopLatch();
    BasicBlock *InnerHeader = Inner->getHeader();
    BasicBlock *InnerLatch = Inner->getLoopLatch();
    MDNode *LoopID = Outer->getLoopID();

    InnerLatch->getTerminator()->replaceSuccessorWith(InnerHeader, OuterHeader);
    for (PHINode &PN : OuterHeader->phis())
        PN.addIncoming(&PN, InnerLatch);

    SmallVector<BasicBlock *, 2> Latches = {OuterLatch, InnerLatch};
    BasicBlock *Latch = SplitBlockPredecessors(OuterHeader, Latches, ".latch",
                                               static_cast<DominatorTree *>(nullptr));
    OuterLatch->getTerminator()->setMetadata(LLVMContext::MD_loop, nullptr);
    InnerLatch->getTerminator()->setMetadata(LLVMContext::MD_loop, nullptr);
    Latch->getTerminator()->setMetadata(LLVMContext::MD_loop, LoopID);
}

} // namespace

PreservedAnalyses UnifyAnnotatedLoopPass::run(Function &F, FunctionAnalysisManager &) {
    bool Changed = false;
    while (true) {
        DominatorTree DT(F);
        LoopInfo LI(DT);
        Loop *Outer = nullptr;
        Loop *Inner = nullptr;
        for (Loop *L : LI.getLoopsInPreorder()) {
            Inner = findSplitInnerLoop(L);
            if (Inner) {
                Outer = L;
                break;
            }
        }
        if (!Outer)
            break;

        PLOGD << "UnifyAnnotatedLoopPass: " << F.getName() << " unified "
              << Outer->getHeader()->getName() << " and " << Inner->getHeader()->getName();
        unifyPair(Outer, Inner);
        Changed = true;
    }
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace checkpoint
