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

    /// Debug counter globals (resolved when addDebugMarkers_ is true).
    /// cnt_save_vreg/cnt_restore_vreg count IR-level value saves which may
    /// not map 1:1 to physical register saves due to register spilling.
    /// TODO: Post-regalloc pass for exact physical register counting.
    llvm::GlobalVariable *cntSaveVregGV_ = nullptr;
    llvm::GlobalVariable *cntRestoreVregGV_ = nullptr;
    llvm::GlobalVariable *cntStoreMemGV_ = nullptr;
    llvm::GlobalVariable *cntRestoreMemGV_ = nullptr;

    /// Maps candidate globals to their SRAM shadow globals.
    std::map<llvm::GlobalVariable *, llvm::GlobalVariable *> shadowMap_;

    /// Maps ineligible objects (globals, allocas, SSA values) to NVM backup globals.
    std::map<llvm::Value *, llvm::GlobalVariable *> nvmBackupMap_;
    /// Per-alloca FRAM slot holding the alloca's runtime address, so restore
    /// code after a boundary reboot can reload it instead of keeping it live
    /// across the boundary in a register or stack spill slot.
    std::map<llvm::Value *, llvm::GlobalVariable *> allocaAddrMap_;

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

    /// Emit a load-add-store sequence to increment a 16-bit debug counter global.
    void emitCounterIncrement(llvm::IRBuilder<> &builder, llvm::GlobalVariable *counter);
};

} // namespace checkpoint
