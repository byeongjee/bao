#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// LLVM pass for MILP-based checkpoint insertion.
/// Analyzes the CFG, solves MILP optimization, and inserts checkpoint calls.
class MILPCheckpointPass : public llvm::PassInfoMixin<MILPCheckpointPass> {
  public:
    /// Run the pass on a function.
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    /// Pass name for registration.
    static llvm::StringRef name() { return "MILPCheckpointPass"; }
};

} // namespace checkpoint
