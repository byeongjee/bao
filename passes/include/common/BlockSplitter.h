#pragma once

#include "common/CFGAnalysis.h"
#include "estimator/EnergyEstimator.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

namespace checkpoint {

/// Split BB at the instruction where cumulative energy first reaches threshold.
/// Returns the new (second half) block, or nullptr if unsplittable.
llvm::BasicBlock *splitOversizedBlock(llvm::BasicBlock *BB,
                                       double threshold,
                                       EnergyEstimator &estimator);

/// Split all oversized blocks in F until every block fits within threshold.
/// Rebuilds cfg after splitting. Returns false if any block is unsplittable.
bool splitAllOversizedBlocks(llvm::Function &F,
                              double threshold,
                              EnergyEstimator &estimator,
                              llvm::LoopInfo &LI,
                              CFGAnalysis &cfg);

} // namespace checkpoint
