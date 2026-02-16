#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Chunks innermost loops using a chunk size derived from energy budget.
/// The pass skips unsupported loops conservatively.
class LoopChunkingPass : public llvm::PassInfoMixin<LoopChunkingPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &F,
                                llvm::FunctionAnalysisManager &AM);

    static llvm::StringRef name() { return "LoopChunkingPass"; }
};

} // namespace checkpoint
