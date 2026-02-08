#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Phase-2 validator for region energy safety.
class MILPValidatePass : public llvm::PassInfoMixin<MILPValidatePass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &F,
                                llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "MILPValidatePass"; }
};

} // namespace checkpoint
