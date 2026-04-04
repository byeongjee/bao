#pragma once

#include "milp/EnergyModel.h"
#include "milp/EnergyPathUtils.h"
#include "milp/StateAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include <string>

namespace checkpoint {

struct StripMiningBudgetResult {
    bool ok = false;
    double pathEnergy = 0.0;
    double perIterNvmPenalty = 0.0;
    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = 0.0;
    double budgetAfterBoundary = 0.0;
    uint64_t maxK = 0;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> pathBlocks;
    std::string reason;
    std::string details;
};

WorstCasePathResult computeStripMiningSummaryPath(
    llvm::Loop *L, const llvm::DenseMap<const llvm::BasicBlock *, double> &blockEnergyByBB,
    llvm::LoopInfo &LI, llvm::ScalarEvolution &SE);

StripMiningBudgetResult computeStripMiningBudgetResult(
    llvm::Loop *L, const llvm::DenseMap<const llvm::BasicBlock *, double> &blockEnergyByBB,
    llvm::LoopInfo &LI, llvm::ScalarEvolution &SE, const StateAnalysis &state,
    const MILPEnergyParams &params);

} // namespace checkpoint
