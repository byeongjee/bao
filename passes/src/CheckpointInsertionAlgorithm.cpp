#include "CheckpointInsertionAlgorithm.h"

#include "MILPNextPass.h"
#include "RockClimbPass.h"

#include <memory>

namespace checkpoint {
namespace {

class MILPCheckpointInsertionAlgorithm final : public CheckpointInsertionAlgorithm {
public:
    llvm::StringRef algorithmName() const override { return "milp"; }

    llvm::PreservedAnalyses run(llvm::Function &F,
                                llvm::FunctionAnalysisManager &AM) override {
        return MILPNextPass().run(F, AM);
    }
};

class RockClimbCheckpointInsertionAlgorithm final
    : public CheckpointInsertionAlgorithm {
public:
    llvm::StringRef algorithmName() const override { return "rockclimb"; }

    llvm::PreservedAnalyses run(llvm::Function &F,
                                llvm::FunctionAnalysisManager &AM) override {
        return RockClimbPass().run(F, AM);
    }
};

} // namespace

std::unique_ptr<CheckpointInsertionAlgorithm>
createCheckpointInsertionAlgorithm(llvm::StringRef algorithmName) {
    if (algorithmName.equals_insensitive("milp")) {
        return std::make_unique<MILPCheckpointInsertionAlgorithm>();
    }
    if (algorithmName.equals_insensitive("rockclimb")) {
        return std::make_unique<RockClimbCheckpointInsertionAlgorithm>();
    }
    return nullptr;
}

} // namespace checkpoint
