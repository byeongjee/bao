#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Dispatcher pass for checkpoint insertion algorithms.
/// Selection is controlled by `-checkpoint-algorithm`.
class CheckpointInsertPass : public llvm::PassInfoMixin<CheckpointInsertPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &F,
                                llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "CheckpointInsertPass"; }
};

} // namespace checkpoint
