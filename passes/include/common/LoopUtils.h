#pragma once

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"

namespace checkpoint {

/// Return true if any block in the loop contains an InvokeInst.
inline bool containsInvoke(const llvm::Loop *L) {
    for (const llvm::BasicBlock *BB : L->blocks())
        for (const llvm::Instruction &I : *BB)
            if (llvm::isa<llvm::InvokeInst>(I))
                return true;
    return false;
}

/// Given a block inside Parent, walk up the loop tree to find
/// the direct child loop of Parent that contains BB.
inline llvm::Loop *getDirectChildLoop(const llvm::Loop *Parent, const llvm::BasicBlock *BB,
                                      const llvm::LoopInfo &LI) {
    llvm::Loop *Inner = LI.getLoopFor(BB);
    if (!Inner || Inner == Parent)
        return nullptr;
    while (Inner->getParentLoop() != Parent)
        Inner = Inner->getParentLoop();
    return Inner;
}

} // namespace checkpoint
