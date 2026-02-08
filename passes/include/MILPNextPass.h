#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Phase-1 bring-up pass for the new MILP pipeline.
/// This pass does not instrument code yet.
class MILPNextPass : public llvm::PassInfoMixin<MILPNextPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &F,
                                llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "MILPNextPass"; }
};

} // namespace checkpoint
