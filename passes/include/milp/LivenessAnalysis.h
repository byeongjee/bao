#pragma once

#include "common/CFGAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Value.h"

#include <set>
#include <vector>

namespace checkpoint {

/// Compute liveness for eligible (candidate) globals using
/// load-before-must-store analysis.
/// Returns BB -> set of live-in GlobalVariables.
llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::GlobalVariable *>>
computeEligibleLiveness(llvm::Function &F, llvm::AAResults &AA, const CFGAnalysis &cfg,
                        const std::vector<llvm::GlobalVariable *> &vmObjs);

/// Compute liveness for ineligible allocas using
/// load-before-must-store analysis.
/// Returns BB -> set of live-in Values.
llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>>
computeIneligAllocaLiveness(llvm::Function &F, const CFGAnalysis &cfg,
                            const std::vector<llvm::Value *> &ineligibleObjs);

/// Compute edge-aware SSA liveness for ineligible cross-block SSA values.
///
/// Uses edge-aware PHI handling: V is live-in at a PHI block (needed for
/// checkpoint correctness), but backward propagation only flows through
/// incoming edges that actually carry V. This prevents liveness from
/// leaking through predecessor edges that carry different PHI operands.
///
/// Returns BB -> set of live-in Values.
llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>>
computeIneligSSALiveness(llvm::Function &F, const CFGAnalysis &cfg,
                         const std::vector<llvm::Value *> &ineligibleObjs);

} // namespace checkpoint
