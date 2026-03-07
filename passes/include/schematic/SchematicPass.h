#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// LLVM pass for SCHEMATIC greedy heuristic checkpoint insertion.
/// Skeleton implementation: runs analyses and prints statistics.
class SchematicPass : public llvm::PassInfoMixin<SchematicPass> {
  public:
    /// Run the pass on a function.
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    /// Pass name for registration.
    static llvm::StringRef name() { return "SchematicPass"; }
};

} // namespace checkpoint
