#pragma once

#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Value.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Compute liveness for eligible (candidate) globals using
/// load-before-must-store analysis.
/// Returns block_name -> set of live-in GlobalVariables.
std::map<std::string, std::set<llvm::GlobalVariable *>>
computeEligibleLiveness(
    llvm::Function &F,
    llvm::AAResults &AA,
    const CFGAnalysis &cfg,
    const std::vector<llvm::GlobalVariable *> &vmObjs,
    const std::map<std::string, llvm::BasicBlock *> &nameToBlock);

/// Compute liveness for ineligible globals and allocas using
/// load-before-must-store analysis.
/// Returns block_name -> set of live-in Values.
std::map<std::string, std::set<llvm::Value *>>
computeIneligGlobalAllocaLiveness(
    llvm::Function &F,
    llvm::AAResults &AA,
    const CFGAnalysis &cfg,
    const std::vector<llvm::Value *> &ineligibleObjs,
    const std::map<std::string, llvm::BasicBlock *> &nameToBlock);

/// Compute edge-aware SSA liveness for ineligible cross-block SSA values.
///
/// Uses edge-aware PHI handling: V is live-in at a PHI block (needed for
/// checkpoint correctness), but backward propagation only flows through
/// incoming edges that actually carry V. This prevents liveness from
/// leaking through predecessor edges that carry different PHI operands.
///
/// Returns block_name -> set of live-in Values.
std::map<std::string, std::set<llvm::Value *>>
computeIneligSSALiveness(
    llvm::Function &F,
    const CFGAnalysis &cfg,
    const std::vector<llvm::Value *> &ineligibleObjs,
    const std::map<std::string, llvm::BasicBlock *> &nameToBlock);

} // namespace checkpoint
