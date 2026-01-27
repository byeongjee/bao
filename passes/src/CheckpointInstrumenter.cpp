#include "CheckpointInstrumenter.h"
#include "BlockUtils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

namespace checkpoint {

CheckpointInstrumenter::CheckpointInstrumenter(llvm::Module &M,
                                               llvm::StringRef checkpointFnName)
    : M_(M) {
    llvm::LLVMContext &Ctx = M_.getContext();

    // Declare: void checkpoint_fn(const char*)
    checkpointCallee_ = M_.getOrInsertFunction(
        checkpointFnName,
        llvm::Type::getVoidTy(Ctx),
        llvm::PointerType::get(Ctx, 0)  // ptr (opaque pointer)
    );
}

void CheckpointInstrumenter::insertCheckpoint(llvm::BasicBlock &BB,
                                              llvm::StringRef blockName) {
    llvm::LLVMContext &Ctx = M_.getContext();

    // Insert after PHI nodes
    llvm::BasicBlock::iterator InsertPt = BB.getFirstNonPHIIt();
    llvm::IRBuilder<> Builder(&*InsertPt);

    // Create global string for block name
    llvm::GlobalVariable *StrGV = Builder.CreateGlobalString(
        blockName, "checkpoint_name");

    // Get pointer to first element using constant expression GEP
    llvm::Constant *Zero = llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(Ctx), 0);
    llvm::Constant *Indices[] = {Zero, Zero};
    llvm::Constant *StrPtr = llvm::ConstantExpr::getInBoundsGetElementPtr(
        StrGV->getValueType(), StrGV, Indices);

    Builder.CreateCall(checkpointCallee_, {StrPtr});
}

unsigned CheckpointInstrumenter::instrumentFunction(
    llvm::Function &F,
    const std::set<std::string> &checkpointBlocks) {

    unsigned count = 0;
    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);
        if (checkpointBlocks.count(blockName)) {
            insertCheckpoint(BB, blockName);
            ++count;
        }
    }
    return count;
}

} // namespace checkpoint
