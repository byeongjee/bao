#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Restores a single latch when LLVM split one annotated source loop into a
/// nested pair. Only the form that needs no PHI synthesis is changed.
class UnifyAnnotatedLoopPass : public llvm::PassInfoMixin<UnifyAnnotatedLoopPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
};

} // namespace checkpoint
