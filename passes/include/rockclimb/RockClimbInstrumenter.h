#pragma once

#include "rockclimb/DistributedCheckpointing.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// RockClimb instrumenter for IR modification.
/// Handles three types of instrumentation:
/// 1. Boundary checks: Insert voltage check calls at region boundaries
/// 2. Register checkpoints: Insert store instructions for distributed checkpointing
/// 3. Memory checkpoints: Insert stores to NVM for allocas and globals
class RockClimbInstrumenter {
public:
    /// Construct instrumenter for a module.
    /// @param M The module to instrument.
    /// @param checkFnName Name of the voltage check function (default: __rockclimb_check).
    /// @param saveRegFnName Name of register save function (default: __rockclimb_save_reg).
    RockClimbInstrumenter(llvm::Module &M,
                          llvm::StringRef checkFnName = "__rockclimb_check",
                          llvm::StringRef saveRegFnName = "__rockclimb_save_reg");

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

    // === Memory checkpointing methods ===

    /// Instrument memory checkpoints at region boundaries.
    /// For each boundary, inserts stores from memory locations to NVM globals.
    /// @param F The function to instrument.
    /// @param memCkpts Memory checkpoint analysis result.
    /// @param boundaries Vector of boundary block names (indexed by boundary ID).
    /// @return Number of memory checkpoint stores inserted.
    unsigned instrumentMemoryCheckpoints(
        llvm::Function &F,
        const MemoryCheckpointResult &memCkpts,
        const std::vector<std::string> &boundaries);

    /// Generate restore function for a specific boundary.
    /// Creates a function that restores all checkpointed memory for that boundary.
    /// @param boundaryId The boundary identifier.
    /// @param ckpts Vector of memory checkpoints for this boundary.
    /// @return The generated restore function.
    llvm::Function* generateRestoreFunction(
        unsigned boundaryId,
        const std::vector<MemoryCheckpointPoint> &ckpts);

    /// Insert recovery dispatcher at function entry.
    /// Checks __nvm_region_id and dispatches to appropriate restore function.
    /// @param F The function to instrument.
    /// @param restoreFns Map from boundary ID to restore function.
    /// @param boundaryBlocks Map from boundary ID to target block after restore.
    void insertRecoveryDispatcher(
        llvm::Function &F,
        const std::map<unsigned, llvm::Function*> &restoreFns,
        const std::map<unsigned, llvm::BasicBlock*> &boundaryBlocks);

    /// Get the NVM slot global for a memory checkpoint (creates if needed).
    /// @param ckpt The memory checkpoint point.
    /// @return The NVM global variable for storing the checkpoint.
    llvm::GlobalVariable* getOrCreateNVMSlot(const MemoryCheckpointPoint &ckpt);

private:
    llvm::Module &M_;
    llvm::FunctionCallee checkCallee_;      // __rockclimb_check
    llvm::FunctionCallee saveRegCallee_;    // __rockclimb_save_reg
    llvm::GlobalVariable *nvmRegsArray_;    // @__nvm_regs array
    llvm::GlobalVariable *nvmRegionId_;     // @__nvm_region_id

    /// Map from NVM slot name to global variable.
    std::map<std::string, llvm::GlobalVariable*> nvmSlots_;

    /// Map from boundary ID to list of checkpoint points for recovery.
    /// Stored during instrumentMemoryCheckpoints for use in insertRecoveryDispatcher.
    std::map<unsigned, std::vector<MemoryCheckpointPoint>> memCkptsByBoundary_;

    /// Declare external runtime symbols if not already present.
    void declareRuntimeSymbols();

    /// Get or create the NVM register array global.
    llvm::GlobalVariable* getOrCreateNVMRegsArray(unsigned numRegs);

    /// Find __rockclimb_check() call in a basic block.
    /// @param BB The basic block to search.
    /// @return The call instruction, or nullptr if not found.
    llvm::CallInst* findRockClimbCheck(llvm::BasicBlock &BB);

    /// Get basic block by name in a function.
    /// @param F The function to search.
    /// @param name The block name.
    /// @return The basic block, or nullptr if not found.
    llvm::BasicBlock* getBlockByName(llvm::Function &F, const std::string &name);

    /// Insert store from memory location to NVM slot.
    /// @param Builder The IR builder positioned at insertion point.
    /// @param ckpt The memory checkpoint point.
    void insertMemoryToNVMStore(llvm::IRBuilder<> &Builder,
                                 const MemoryCheckpointPoint &ckpt);
};

} // namespace checkpoint
