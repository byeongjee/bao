#include "milp/CheckpointInstrumenter.h"
#include "common/BlockUtils.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

namespace checkpoint {

CheckpointInstrumenter::CheckpointInstrumenter(llvm::Module &M) : M_(M) {
    declareRuntimeFunctions();
}

void CheckpointInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
    llvm::Type *PtrTy = llvm::PointerType::get(Ctx, 0);

    prologueFn_ = M_.getOrInsertFunction("__region_prologue", VoidTy);
    epilogueFn_ = M_.getOrInsertFunction("__region_epilogue", VoidTy);
    storeMemFn_ = M_.getOrInsertFunction(
        "__checkpoint_store_mem", VoidTy, PtrTy, PtrTy, I32Ty);
    restoreMemFn_ = M_.getOrInsertFunction(
        "__restore_mem", VoidTy, PtrTy, PtrTy, I32Ty);
}

unsigned CheckpointInstrumenter::instrumentFunction(
    llvm::Function &F,
    const MILPSolution &solution,
    const StateAnalysis &state) {

    unsigned inserted = 0;
    inserted += insertRegionBoundaries(F, solution, state);
    applyMemoryPlacement(state);
    return inserted;
}

unsigned CheckpointInstrumenter::insertRegionBoundaries(
    llvm::Function &F,
    const MILPSolution &solution,
    const StateAnalysis &state) {

    unsigned inserted = 0;
    const std::string &entryName = getBlockName(F.getEntryBlock(), F);

    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);
        if (!solution.regionStarts.count(blockName)) {
            continue;
        }

        llvm::BasicBlock::iterator insertIt = BB.getFirstNonPHIIt();
        if (insertIt == BB.end()) {
            continue;
        }

        llvm::IRBuilder<> builder(&BB, insertIt);

        // For b != b0, emit boundary-end code first.
        if (blockName != entryName) {
            builder.CreateCall(epilogueFn_);
            inserted++;

            for (llvm::GlobalVariable *GV : state.getVMObjLiveIn(blockName)) {
                auto key = std::make_pair(blockName, GV);
                auto it = solution.commit.find(key);
                if (it == solution.commit.end() || !it->second) {
                    continue;
                }

                int elemId = state.getVMObjStateElemId(GV);
                if (elemId < 0) {
                    continue;
                }
                const StateElement &elem =
                    state.getStateElement(static_cast<unsigned>(elemId));
                llvm::Value *size = llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(M_.getContext()), elem.sizeBytes);
                builder.CreateCall(storeMemFn_, {GV, GV, size});
                inserted++;
            }
        }

        // Then emit boundary-start code.
        builder.CreateCall(prologueFn_);
        inserted++;

        for (llvm::GlobalVariable *GV : state.getVMObjLiveIn(blockName)) {
            auto key = std::make_pair(blockName, GV);
            auto it = solution.needRestore.find(key);
            if (it == solution.needRestore.end() || !it->second) {
                continue;
            }

            int elemId = state.getVMObjStateElemId(GV);
            if (elemId < 0) {
                continue;
            }
            const StateElement &elem =
                state.getStateElement(static_cast<unsigned>(elemId));
            llvm::Value *size = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(M_.getContext()), elem.sizeBytes);
            builder.CreateCall(restoreMemFn_, {GV, GV, size});
            inserted++;
        }
    }

    return inserted;
}

void CheckpointInstrumenter::applyMemoryPlacement(const StateAnalysis &state) {
    // Candidate globals are placed in a dedicated section for runtime handling.
    for (llvm::GlobalVariable *GV : state.getVMObjs()) {
        GV->setSection(".candidate");
    }
}

} // namespace checkpoint
