#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// LLVM pass that instruments a function for execution trace collection.
/// Inserts calls to a C runtime that records per-function and per-loop
/// traces, then writes them as JSON via atexit.
class TraceCollectorPass : public llvm::PassInfoMixin<TraceCollectorPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "TraceCollectorPass"; }
};

} // namespace checkpoint
