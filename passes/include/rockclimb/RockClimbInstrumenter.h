#pragma once

#include "rockclimb/DistributedCheckpointing.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/StringRef.h"

#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// RockClimb instrumenter for IR modification.
/// Handles two types of instrumentation:
/// 1. Boundary checks: Insert voltage check calls at region boundaries
/// 2. Register checkpoints: Insert store instructions for distributed checkpointing
class RockClimbInstrumenter {
public:
    /// Construct instrumenter for a module.
    /// @param M The module to instrument.
    /// @param checkFnName Name of the voltage check function (default: __rockclimb_check).
    /// @param saveRegFnName Name of register save function (default: __rockclimb_save_reg).
    /// @param addDebugMarkers Emit MILP-style debug marker calls for counters.
    RockClimbInstrumenter(llvm::Module &M,
                          llvm::StringRef checkFnName = "__rockclimb_check",
                          llvm::StringRef saveRegFnName = "__rockclimb_save_reg",
                          bool addDebugMarkers = false);

    /// Insert boundary check at the start of a basic block.
    /// The check is inserted after any PHI nodes.
    /// @param BB The basic block to instrument.
    void insertBoundaryCheck(llvm::BasicBlock &BB);

    /// Insert register checkpoint (store to NVM).
    /// @param ckpt The checkpoint point information.
    void insertRegisterCheckpoint(const CheckpointPoint &ckpt);

    /// Full instrumentation: boundaries + optional distributed checkpoints.
    /// @param F The function to instrument.
    /// @param boundaries Set of block names where boundaries exist.
    /// @param checkpoints Vector of checkpoint points for distributed checkpointing.
    /// @param enableDistributedCkpt If false, skip register checkpoints.
    /// @return Number of instrumentations inserted.
    unsigned instrumentFunction(
        llvm::Function &F,
        const std::set<std::string> &boundaries,
        const std::vector<CheckpointPoint> &checkpoints,
        bool enableDistributedCkpt);

    /// Declare NVM globals for register storage.
    /// @param numRegs Number of registers to allocate storage for.
    void declareNVMStorage(unsigned numRegs);

private:
    llvm::Module &M_;
    bool addDebugMarkers_;
    llvm::FunctionCallee checkCallee_;      // __rockclimb_check
    llvm::FunctionCallee saveRegCallee_;    // __rockclimb_save_reg
    llvm::FunctionCallee prologueCallee_;   // __region_prologue
    llvm::FunctionCallee epilogueCallee_;   // __region_epilogue
    llvm::FunctionCallee markerStoreRegCallee_; // __checkpoint_store_reg
    llvm::GlobalVariable *nvmRegsArray_;    // @__nvm_regs array

    /// Get or create the NVM register array global.
    llvm::GlobalVariable* getOrCreateNVMRegsArray(unsigned numRegs);

    /// Convert a value to i64 for debug marker calls.
    llvm::Value *convertToI64(llvm::IRBuilder<> &Builder, llvm::Value *V);
};

} // namespace checkpoint
