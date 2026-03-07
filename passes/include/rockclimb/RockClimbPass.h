#pragma once

#include "llvm/IR/PassManager.h"

#include <string>

namespace checkpoint {

/// LLVM pass for RockClimb checkpoint insertion.
/// Implements PFI+RockClimb: greedy region partitioning with distributed checkpointing.
/// Unlike MILP-based approach, uses topological traversal with energy threshold.
class RockClimbPass : public llvm::PassInfoMixin<RockClimbPass> {
  public:
    /// Run the pass on a function.
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    /// Pass name for registration.
    static llvm::StringRef name() { return "RockClimbPass"; }
};

} // namespace checkpoint
