#pragma once

#include "milp/CheckpointOptimizer.h"
#include "milp/ModelViews.h"
#include "milp/StateAnalysis.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"

#include <map>

namespace checkpoint {

/// Instruments LLVM IR based on MILP solution.
class CheckpointInstrumenter {
  public:
    explicit CheckpointInstrumenter(llvm::Module &M, bool addDebugMarkers = false);

    /// Instrument a function using the MILP solution and state analysis.
    unsigned instrumentFunction(llvm::Function &F, const MILPSolution &solution,
                                const ICFGView &cfg, const IStateView &stateView,
                                const StateAnalysis &state);

  private:
    llvm::Module &M_;
    bool addDebugMarkers_;

    llvm::FunctionCallee prologueFn_;
    llvm::FunctionCallee epilogueFn_;
    llvm::FunctionCallee storeMemFn_;
    llvm::FunctionCallee restoreMemFn_;
    llvm::FunctionCallee storeRegFn_;
    llvm::FunctionCallee restoreRegFn_;

    unsigned slotCounter_ = 0;

    /// Maps candidate globals to their SRAM shadow globals.
    std::map<llvm::GlobalVariable *, llvm::GlobalVariable *> shadowMap_;

    /// Maps ineligible objects (globals, allocas, SSA values) to NVM backup globals.
    std::map<llvm::Value *, llvm::GlobalVariable *> nvmBackupMap_;

    void declareRuntimeFunctions();

    void createShadowGlobals(llvm::Function &F, const MILPSolution &solution,
                             const StateAnalysis &state);
    void createNVMBackupGlobals(llvm::Function &F, const MILPSolution &solution,
                                const StateAnalysis &state, const ICFGView &cfg);
    void rewriteAccessesInVMRegions(llvm::Function &F, const MILPSolution &solution,
                                    const ICFGView &cfg);

    unsigned insertRegionBoundaries(llvm::Function &F, const MILPSolution &solution,
                                    const ICFGView &cfg, const IStateView &stateView,
                                    const StateAnalysis &state);
    void applyMemoryPlacement(const StateAnalysis &state);

    llvm::Value *convertToI64(llvm::IRBuilder<> &builder, llvm::Value *V);
};

} // namespace checkpoint
