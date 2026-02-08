#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// LLVM pass that analyzes checkpoint placement and estimates maximum
/// checkpoints on any execution path.
///
/// This pass:
/// 1. Runs the MILP checkpoint optimizer to get checkpoint locations
/// 2. Extracts loop bounds from __loop_tripcount() marker calls
/// 3. Uses DP to count max checkpoints on any path with bounded iterations
///
/// Output is printed to stderr.
class CheckpointAnalysisPass : public llvm::PassInfoMixin<CheckpointAnalysisPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &F,
                                 llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "CheckpointAnalysisPass"; }
};

} // namespace checkpoint
