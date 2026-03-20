#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// Splits all edges whose destination has more than one predecessor,
/// inserting a new empty basic block on each such edge.  After this
/// pass, every immediate predecessor of a merge point has exactly one
/// predecessor itself, making placement/dirty state unambiguous for
/// downstream checkpoint optimization.
///
/// Merge points still exist — this pass does NOT eliminate them.
/// Downstream passes must forbid region boundaries at merge points.
class EdgeSplitPass : public llvm::PassInfoMixin<EdgeSplitPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "EdgeSplitPass"; }
};

} // namespace checkpoint
