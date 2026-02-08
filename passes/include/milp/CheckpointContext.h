#ifndef CHECKPOINT_CONTEXT_H
#define CHECKPOINT_CONTEXT_H

#include "common/BaseContext.h"

namespace checkpoint {

/// CheckpointContext is an alias for BaseContext — MILP needs no extra fields.
using CheckpointContext = BaseContext;

/// Result type for checkpoint context creation.
using CheckpointContextResult = ContextResult<CheckpointContext>;

/// Create checkpoint context from function and config path.
/// Thin wrapper around createBaseContext with the MILP pass name.
inline CheckpointContextResult createCheckpointContext(
    llvm::Function &F,
    llvm::LoopInfo &LI,
    llvm::StringRef configPath,
    llvm::StringRef passName) {

    return createBaseContext(F, LI, configPath, passName);
}

} // namespace checkpoint

#endif // CHECKPOINT_CONTEXT_H
