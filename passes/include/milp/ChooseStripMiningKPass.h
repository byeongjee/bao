#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

class ChooseStripMiningKPass : public llvm::PassInfoMixin<ChooseStripMiningKPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    static llvm::StringRef name() { return "ChooseStripMiningKPass"; }
};

} // namespace checkpoint
