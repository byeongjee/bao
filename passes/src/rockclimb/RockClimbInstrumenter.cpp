#include "rockclimb/RockClimbInstrumenter.h"
#include "common/BlockUtils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

#include <iterator>

namespace checkpoint {
namespace {

static llvm::BasicBlock::iterator getBoundaryInsertPoint(llvm::BasicBlock &BB) {
    llvm::BasicBlock::iterator insertPt = BB.getFirstNonPHIIt();
    while (insertPt != BB.end() && llvm::isa<llvm::AllocaInst>(&*insertPt)) {
        ++insertPt;
    }
    if (insertPt == BB.end()) {
        insertPt = std::prev(BB.end());
    }
    return insertPt;
}

} // namespace

RockClimbInstrumenter::RockClimbInstrumenter(llvm::Module &M,
                                             llvm::StringRef checkFnName,
                                             llvm::StringRef saveRegFnName,
                                             bool addDebugMarkers)
    : M_(M), addDebugMarkers_(addDebugMarkers), nvmRegsArray_(nullptr) {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);

    // Declare: void __rockclimb_check(void)
    checkCallee_ = M_.getOrInsertFunction(
        checkFnName,
        VoidTy
    );

    // Declare: void __rockclimb_save_reg(uint8_t reg_id, uint16_t value)
    // For LLVM IR, we use i32 for both to be safe across platforms
    saveRegCallee_ = M_.getOrInsertFunction(
        saveRegFnName,
        VoidTy,
        I32Ty,  // reg_id
        I32Ty   // value (will be truncated/extended as needed)
    );

    if (addDebugMarkers_) {
        llvm::Type *I64Ty = llvm::Type::getInt64Ty(Ctx);
        prologueCallee_ = M_.getOrInsertFunction("__region_prologue", VoidTy);
        epilogueCallee_ = M_.getOrInsertFunction("__region_epilogue", VoidTy);
        markerStoreRegCallee_ = M_.getOrInsertFunction(
            "__checkpoint_store_reg", VoidTy, I32Ty, I64Ty);
    }
}

llvm::GlobalVariable* RockClimbInstrumenter::getOrCreateNVMRegsArray(
    unsigned numRegs) {
    if (nvmRegsArray_ && nvmRegsArray_->getValueType()->getArrayNumElements() >= numRegs) {
        return nvmRegsArray_;
    }

    llvm::LLVMContext &Ctx = M_.getContext();

    // Create @__nvm_regs[numRegs] as external global
    // Type: [numRegs x i32]
    llvm::ArrayType *arrayType = llvm::ArrayType::get(
        llvm::Type::getInt32Ty(Ctx), numRegs);

    nvmRegsArray_ = new llvm::GlobalVariable(
        M_,
        arrayType,
        false,  // not constant
        llvm::GlobalValue::ExternalLinkage,
        nullptr,  // external - no initializer
        "__nvm_regs"
    );

    return nvmRegsArray_;
}

void RockClimbInstrumenter::declareNVMStorage(unsigned numRegs) {
    getOrCreateNVMRegsArray(numRegs);
}

void RockClimbInstrumenter::insertBoundaryCheck(llvm::BasicBlock &BB) {
    llvm::BasicBlock::iterator InsertPt = getBoundaryInsertPoint(BB);
    llvm::IRBuilder<> Builder(&*InsertPt);

    // Marker order mirrors MILP transition semantics at a boundary:
    // epilogue (end old region) -> prologue (start new region).
    if (addDebugMarkers_) {
        Builder.CreateCall(epilogueCallee_, {});
        Builder.CreateCall(prologueCallee_, {});
    }

    // Call __rockclimb_check()
    Builder.CreateCall(checkCallee_, {});
}

void RockClimbInstrumenter::insertRegisterCheckpoint(
    const CheckpointPoint &ckpt) {
    llvm::LLVMContext &Ctx = M_.getContext();

    // Insert AFTER the instruction that defines the register
    llvm::Instruction *afterInst = ckpt.afterInst;

    // Handle case where instruction is terminator
    if (afterInst->isTerminator()) {
        // Can't insert after terminator in same block
        // This shouldn't happen for normal definitions, but handle gracefully
        return;
    }

    llvm::BasicBlock::iterator insertPt;
    if (llvm::isa<llvm::PHINode>(afterInst)) {
        // PHIs must stay grouped at block start; insert after the last PHI
        insertPt = afterInst->getParent()->getFirstNonPHIIt();
    } else {
        llvm::BasicBlock::iterator it(afterInst);
        ++it;
        insertPt = it;
    }

    llvm::IRBuilder<> Builder(&*insertPt);

    // Get or extend the value to i32 for the call
    llvm::Value *regValue = ckpt.reg;
    llvm::Type *regType = regValue->getType();

    llvm::Value *valueToStore;
    if (regType->isIntegerTy()) {
        unsigned bitWidth = regType->getIntegerBitWidth();
        if (bitWidth < 32) {
            valueToStore = Builder.CreateZExt(regValue,
                                              llvm::Type::getInt32Ty(Ctx));
        } else if (bitWidth > 32) {
            valueToStore = Builder.CreateTrunc(regValue,
                                               llvm::Type::getInt32Ty(Ctx));
        } else {
            valueToStore = regValue;
        }
    } else if (regType->isPointerTy()) {
        // Convert pointer to integer
        valueToStore = Builder.CreatePtrToInt(regValue,
                                               llvm::Type::getInt32Ty(Ctx));
    } else if (regType->isFloatingPointTy()) {
        // Bitcast float to int
        if (regType->isFloatTy()) {
            valueToStore = Builder.CreateBitCast(regValue,
                                                  llvm::Type::getInt32Ty(Ctx));
        } else {
            // For double, truncate to float first then bitcast
            llvm::Value *asFloat = Builder.CreateFPTrunc(
                regValue, llvm::Type::getFloatTy(Ctx));
            valueToStore = Builder.CreateBitCast(asFloat,
                                                  llvm::Type::getInt32Ty(Ctx));
        }
    } else {
        // Unsupported type - skip
        return;
    }

    // Create reg_id constant
    llvm::Value *regIdVal = llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(Ctx), ckpt.regId);

    // Call __rockclimb_save_reg(reg_id, value)
    Builder.CreateCall(saveRegCallee_, {regIdVal, valueToStore});

    if (addDebugMarkers_) {
        llvm::Value *valueAsI64 = convertToI64(Builder, regValue);
        Builder.CreateCall(markerStoreRegCallee_, {regIdVal, valueAsI64});
    }
}

unsigned RockClimbInstrumenter::instrumentFunction(
    llvm::Function &F,
    const llvm::SmallPtrSet<llvm::BasicBlock*, 16> &boundaries,
    const std::vector<CheckpointPoint> &checkpoints,
    bool enableDistributedCkpt) {

    unsigned count = 0;

    if (addDebugMarkers_) {
        llvm::BasicBlock &Entry = F.getEntryBlock();
        llvm::BasicBlock::iterator InsertPt = getBoundaryInsertPoint(Entry);
        llvm::IRBuilder<> Builder(&*InsertPt);
        Builder.CreateCall(prologueCallee_, {});
    }

    // Insert boundary checks
    for (llvm::BasicBlock &BB : F) {
        if (boundaries.count(&BB)) {
            insertBoundaryCheck(BB);
            ++count;
        }
    }

    // Insert distributed register checkpoints if enabled
    if (enableDistributedCkpt && !checkpoints.empty()) {
        declareNVMStorage(checkpoints.size());

        for (const auto &ckpt : checkpoints) {
            insertRegisterCheckpoint(ckpt);
            ++count;
        }
    }

    return count;
}

llvm::Value *RockClimbInstrumenter::convertToI64(llvm::IRBuilder<> &Builder,
                                                 llvm::Value *V) {
    llvm::Type *Ty = V->getType();
    llvm::Type *I64Ty = llvm::Type::getInt64Ty(M_.getContext());

    if (Ty->isIntegerTy()) {
        unsigned bits = Ty->getIntegerBitWidth();
        if (bits < 64)
            return Builder.CreateZExt(V, I64Ty);
        if (bits == 64)
            return V;
        return Builder.CreateTrunc(V, I64Ty);
    }
    if (Ty->isPointerTy())
        return Builder.CreatePtrToInt(V, I64Ty);
    if (Ty->isFloatTy()) {
        llvm::Value *asI32 = Builder.CreateBitCast(
            V, llvm::Type::getInt32Ty(M_.getContext()));
        return Builder.CreateZExt(asI32, I64Ty);
    }
    if (Ty->isDoubleTy())
        return Builder.CreateBitCast(V, I64Ty);

    return llvm::ConstantInt::get(I64Ty, 0);
}

} // namespace checkpoint
