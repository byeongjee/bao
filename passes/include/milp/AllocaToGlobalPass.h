#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Rewrites main's allocas into internal globals in .fram.
///
/// MILP links with the stack in SRAM, so a live stack object is copied to
/// FRAM and back at every region boundary.  A .fram global is durable on its
/// own, and MILP models globals as placeable, so the copies become optional.
///
/// Only main: a global is one object per program, an alloca one per
/// invocation, and main's frame is never live twice.
class AllocaToGlobalPass : public llvm::PassInfoMixin<AllocaToGlobalPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    static llvm::StringRef name() { return "AllocaToGlobalPass"; }
};

} // namespace checkpoint
