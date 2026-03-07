#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Strip-mines loops using a chunk size derived from energy budget.
/// The pass skips unsupported loops conservatively.
class LoopStripMiningPass : public llvm::PassInfoMixin<LoopStripMiningPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    static llvm::StringRef name() { return "LoopStripMiningPass"; }
};

} // namespace checkpoint
