#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

class RockClimbLoopUnrollPass : public llvm::PassInfoMixin<RockClimbLoopUnrollPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    static llvm::StringRef name() { return "RockClimbLoopUnrollPass"; }
};

} // namespace checkpoint
