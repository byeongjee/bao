#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Strip-mines loops using a chunk size derived from energy budget.
/// The pass skips unsupported loops conservatively.
class LoopStripMiningPass : public llvm::PassInfoMixin<LoopStripMiningPass> {
  public:
    explicit LoopStripMiningPass(bool reclampOnly = false) : reclampOnly_(reclampOnly) {}

    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    static llvm::StringRef name() { return "LoopStripMiningPass"; }

  private:
    bool reclampOnly_;
};

} // namespace checkpoint
