#pragma once

#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace checkpoint {

/// Worst-case (maximum-energy) single-iteration path through a loop body.
struct WorstCasePath {
    bool ok = false;
    double energy = 0.0;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> blocksOnPath;
    std::string error;
};

/// Worst-case single-iteration energy path through loop L: the acyclic
/// header-to-latch path maximizing energy, with each direct sub-loop collapsed
/// to its own worst-case iteration energy times subLoopTripCount(SubL).
WorstCasePath
computeWorstCasePath(llvm::Loop *L,
                     const llvm::DenseMap<const llvm::BasicBlock *, double> &blockEnergy,
                     llvm::LoopInfo &LI, llvm::ScalarEvolution &SE,
                     llvm::function_ref<std::optional<uint64_t>(llvm::Loop *)> subLoopTripCount);

/// Sub-loop trip count policy for loop summaries:
/// min(SCEV small constant trip count, marker trip count metadata).
std::optional<uint64_t> summarySubLoopTripCount(llvm::Loop *SubL, llvm::ScalarEvolution &SE);

/// Worst-case iteration path with the summary sub-loop trip count policy.
WorstCasePath
computeWorstCaseSummaryPath(llvm::Loop *L,
                            const llvm::DenseMap<const llvm::BasicBlock *, double> &blockEnergy,
                            llvm::LoopInfo &LI, llvm::ScalarEvolution &SE);

/// The energy budget for summarizing a loop, split into the K-independent
/// pieces of the check. AbstractCFG summarizes a loop with trip count K only
/// if
///     fixedEnergy() + K * perIterEnergy() < budgetAfterBoundary   (strict)
/// and the LoopStripMiningPass re-clamp derives the maximal admissible chunk K
/// by inverting that same inequality. Both sides must take these pieces from
/// computeSummaryBudget: any independently re-derived variant of this
/// accounting can drift out of sync and silently un-summarize strip-mined
/// loops (the re-clamp would then pick a K the summarizer rejects).
struct SummaryBudget {
    bool ok = false;
    std::string error;
    WorstCasePath worstCasePath;
    /// NVM access penalty accrued along the path, once per iteration.
    double perIterNvmPenalty = 0.0;
    /// Preheader energy + NVM penalty, charged once per loop entry: the
    /// summary's boundary sits at the top of the preheader, so the preheader
    /// belongs to the summary.
    double preheaderEnergy = 0.0;
    double preheaderNvmPenalty = 0.0;
    /// Boundary state margin over path + preheader blocks.
    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = 0.0;
    /// (capacity - E_pro - E_epi) - boundaryStateMargin.
    double budgetAfterBoundary = 0.0;

    double perIterEnergy() const { return worstCasePath.energy + perIterNvmPenalty; }
    double fixedEnergy() const { return preheaderEnergy + preheaderNvmPenalty; }
};

SummaryBudget
computeSummaryBudget(llvm::Loop *L,
                     const llvm::DenseMap<const llvm::BasicBlock *, double> &blockEnergy,
                     llvm::LoopInfo &LI, llvm::ScalarEvolution &SE, const StateAnalysis &state,
                     const MILPEnergyParams &params);

/// Compute the save (commit) cost for a single tracked value.
/// Heuristic: ignores non-scalar globals (arrays, structs).
inline double getSaveCostForValue(llvm::Value *V, const StateAnalysis &state,
                                  const MILPEnergyParams &params) {
    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
        llvm::Type *Ty = GV->getValueType();
        if (Ty->isArrayTy() || Ty->isStructTy())
            return 0.0;
    }
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
        if (!llvm::isa<llvm::AllocaInst>(I))
            return params.regStoreEnergy;
    }
    unsigned sizeBytes = state.getVarSizeBytes(V);
    if (sizeBytes == 0)
        return 0.0;
    return static_cast<double>(sizeBytes) * params.memStoreEnergyPerByte;
}

/// Compute the restore cost for a single tracked value.
/// Heuristic: ignores non-scalar globals (arrays, structs).
inline double getRestoreCostForValue(llvm::Value *V, const StateAnalysis &state,
                                     const MILPEnergyParams &params) {
    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
        llvm::Type *Ty = GV->getValueType();
        if (Ty->isArrayTy() || Ty->isStructTy())
            return 0.0;
    }
    if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
        if (!llvm::isa<llvm::AllocaInst>(I))
            return params.regRestoreEnergy;
    }
    unsigned sizeBytes = state.getVarSizeBytes(V);
    if (sizeBytes == 0)
        return 0.0;
    return static_cast<double>(sizeBytes) * params.memRestoreEnergyPerByte;
}

/// Compute the total boundary-state energy margin on a path:
/// restore cost for all live-in variables + commit cost for all defined variables.
/// q (reboot probability) is intentionally treated as 1.0 for loop-level budgeting.
inline double
computeBoundaryStateMarginOnPath(const llvm::SmallPtrSetImpl<const llvm::BasicBlock *> &pathBlocks,
                                 const StateAnalysis &state, const MILPEnergyParams &params,
                                 double &restoreLiveInMargin, double &commitDefMargin) {
    std::set<llvm::Value *> liveInVars;
    std::set<llvm::Value *> defVars;

    for (const llvm::BasicBlock *BB : pathBlocks) {
        for (llvm::GlobalVariable *GV : state.getEligLiveIn(BB))
            liveInVars.insert(GV);
        for (llvm::Value *V : state.getIneligLiveIn(BB))
            liveInVars.insert(V);

        for (llvm::GlobalVariable *GV : state.getVMObjs()) {
            if (state.getEligDefIndicator(BB, GV))
                defVars.insert(GV);
        }
        for (llvm::Value *V : state.getIneligibleObjs()) {
            if (state.getIneligDefIndicator(BB, V))
                defVars.insert(V);
        }
    }

    restoreLiveInMargin = 0.0;
    for (llvm::Value *V : liveInVars)
        restoreLiveInMargin += getRestoreCostForValue(V, state, params);

    commitDefMargin = 0.0;
    for (llvm::Value *V : defVars)
        commitDefMargin += getSaveCostForValue(V, state, params);

    return restoreLiveInMargin + commitDefMargin;
}

} // namespace checkpoint
