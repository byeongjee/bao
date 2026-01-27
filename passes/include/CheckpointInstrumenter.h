#ifndef CHECKPOINT_INSTRUMENTER_H
#define CHECKPOINT_INSTRUMENTER_H

#include "llvm/IR/Module.h"
#include "llvm/ADT/StringRef.h"

#include <set>
#include <string>

namespace checkpoint {

/// Inserts checkpoint function calls into basic blocks.
/// Handles function declaration and call site generation.
class CheckpointInstrumenter {
public:
    /// Construct instrumenter for a module.
    /// @param M The module to instrument.
    /// @param checkpointFnName Name of the checkpoint function to call.
    CheckpointInstrumenter(llvm::Module &M, llvm::StringRef checkpointFnName);

    /// Insert checkpoint call at the beginning of a basic block.
    /// The call is inserted after any PHI nodes.
    /// @param BB The basic block to instrument.
    /// @param blockName Name to pass to the checkpoint function.
    void insertCheckpoint(llvm::BasicBlock &BB, llvm::StringRef blockName);

    /// Instrument a function by inserting checkpoints at specified blocks.
    /// @param F The function to instrument.
    /// @param checkpointBlocks Set of block names to checkpoint.
    /// @return Number of checkpoints inserted.
    unsigned instrumentFunction(llvm::Function &F,
                                const std::set<std::string> &checkpointBlocks);

private:
    llvm::Module &M_;
    llvm::FunctionCallee checkpointCallee_;
};

} // namespace checkpoint

#endif // CHECKPOINT_INSTRUMENTER_H
