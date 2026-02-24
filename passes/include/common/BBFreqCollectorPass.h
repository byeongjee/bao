#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// LLVM pass that instruments a function for basic block frequency collection.
/// Inserts per-BB counter increments (load/add/store) and calls to a C runtime
/// that registers counters and dumps them as JSON on exit.
class BBFreqCollectorPass : public llvm::PassInfoMixin<BBFreqCollectorPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &F,
                                 llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "BBFreqCollectorPass"; }
};

} // namespace checkpoint
