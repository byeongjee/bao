#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Converts __loop_tripcount() function calls into !llvm.loop metadata,
/// then erases the calls.  Must run before any optimization passes that
/// could move or eliminate the marker calls.
class TripCountAnnotationPass : public llvm::PassInfoMixin<TripCountAnnotationPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    static llvm::StringRef name() { return "TripCountAnnotationPass"; }
};

} // namespace checkpoint
