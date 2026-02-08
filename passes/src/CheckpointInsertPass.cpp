#include "CheckpointInsertPass.h"

#include "CheckpointInsertionAlgorithm.h"
#include "MILPOptions.h"

#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

namespace checkpoint {

llvm::PreservedAnalyses CheckpointInsertPass::run(
    llvm::Function &F,
    llvm::FunctionAnalysisManager &AM) {
    auto algorithm = createCheckpointInsertionAlgorithm(
        CheckpointAlgorithmOpt.getValue());
    if (!algorithm) {
        llvm::errs() << "Error: unsupported checkpoint algorithm '"
                     << CheckpointAlgorithmOpt.getValue()
                     << "'. Supported values: milp, rockclimb\n";
        return llvm::PreservedAnalyses::all();
    }

    llvm::errs() << "checkpoint-insert: algorithm='"
                 << algorithm->algorithmName() << "' on function '"
                 << F.getName() << "'\n";
    return algorithm->run(F, AM);
}

} // namespace checkpoint
