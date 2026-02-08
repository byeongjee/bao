#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

#include <memory>

namespace checkpoint {

/// Abstract algorithm contract for checkpoint insertion.
/// Implementations operate on LLVM IR and may modify the function in place.
class CheckpointInsertionAlgorithm {
public:
    virtual ~CheckpointInsertionAlgorithm() = default;

    /// Algorithm identifier used for diagnostics.
    virtual llvm::StringRef algorithmName() const = 0;

    /// Run checkpoint insertion for the provided function.
    virtual llvm::PreservedAnalyses run(llvm::Function &F,
                                        llvm::FunctionAnalysisManager &AM) = 0;
};

/// Construct an algorithm implementation by name.
/// Supported names in v1: "milp", "rockclimb".
std::unique_ptr<CheckpointInsertionAlgorithm>
createCheckpointInsertionAlgorithm(llvm::StringRef algorithmName);

} // namespace checkpoint
