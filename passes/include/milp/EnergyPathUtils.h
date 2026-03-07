#pragma once

#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

#include <set>
#include <string>

namespace checkpoint {

/// Result of a worst-case energy path computation through a loop body.
struct WorstCasePathResult {
    bool ok = false;
    double energy = 0.0;
    llvm::SmallPtrSet<const llvm::BasicBlock *, 16> blocksOnPath;
    std::string error;
};

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
