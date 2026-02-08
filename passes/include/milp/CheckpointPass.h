#pragma once

#include "llvm/IR/PassManager.h"

#include <string>

namespace checkpoint {

/// LLVM pass for checkpoint insertion.
/// Analyzes the CFG, solves MILP optimization, and inserts checkpoint calls.
class CheckpointPass : public llvm::PassInfoMixin<CheckpointPass> {
public:
    /// Run the pass on a function.
    llvm::PreservedAnalyses run(llvm::Function &F,
                                 llvm::FunctionAnalysisManager &AM);

    /// Pass name for registration.
    static llvm::StringRef name() { return "CheckpointPass"; }
};

} // namespace checkpoint
