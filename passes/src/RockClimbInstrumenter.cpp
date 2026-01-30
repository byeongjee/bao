#include "RockClimbInstrumenter.h"
#include "BlockUtils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"

namespace checkpoint {

RockClimbInstrumenter::RockClimbInstrumenter(llvm::Module &M,
                                             llvm::StringRef checkFnName,
                                             llvm::StringRef saveRegFnName)
    : M_(M), nvmRegsArray_(nullptr), nvmRegionId_(nullptr) {
    llvm::LLVMContext &Ctx = M_.getContext();

    // Declare: void __rockclimb_check(void)
    checkCallee_ = M_.getOrInsertFunction(
        checkFnName,
        llvm::Type::getVoidTy(Ctx)
    );

    // Declare: void __rockclimb_save_reg(uint8_t reg_id, uint16_t value)
    // For LLVM IR, we use i32 for both to be safe across platforms
    saveRegCallee_ = M_.getOrInsertFunction(
        saveRegFnName,
        llvm::Type::getVoidTy(Ctx),
        llvm::Type::getInt32Ty(Ctx),  // reg_id
        llvm::Type::getInt32Ty(Ctx)   // value (will be truncated/extended as needed)
    );
}

void RockClimbInstrumenter::declareRuntimeSymbols() {
    llvm::LLVMContext &Ctx = M_.getContext();

    // Declare @__nvm_region_id as external global if not exists
    if (!nvmRegionId_) {
        nvmRegionId_ = M_.getGlobalVariable("__nvm_region_id");
        if (!nvmRegionId_) {
            nvmRegionId_ = new llvm::GlobalVariable(
                M_,
                llvm::Type::getInt32Ty(Ctx),
                false,  // not constant
                llvm::GlobalValue::ExternalLinkage,
                nullptr,  // external - no initializer
                "__nvm_region_id"
            );
        }
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
    declareRuntimeSymbols();
    getOrCreateNVMRegsArray(numRegs);
}

void RockClimbInstrumenter::insertBoundaryCheck(llvm::BasicBlock &BB) {
    // Insert after PHI nodes
    llvm::BasicBlock::iterator InsertPt = BB.getFirstNonPHIIt();
    llvm::IRBuilder<> Builder(&*InsertPt);

    // Call __rockclimb_check()
    Builder.CreateCall(checkCallee_, {});
}

void RockClimbInstrumenter::insertRegisterCheckpoint(
    const CheckpointPoint &ckpt) {
    llvm::LLVMContext &Ctx = M_.getContext();

    // Insert AFTER the instruction that defines the register
    llvm::Instruction *afterInst = ckpt.afterInst;
    llvm::BasicBlock::iterator insertPt(afterInst);
    ++insertPt;  // Move to after the instruction

    // Handle case where instruction is terminator
    if (afterInst->isTerminator()) {
        // Can't insert after terminator in same block
        // This shouldn't happen for normal definitions, but handle gracefully
        return;
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
}

unsigned RockClimbInstrumenter::instrumentFunction(
    llvm::Function &F,
    const std::set<std::string> &boundaries,
    const std::vector<CheckpointPoint> &checkpoints,
    bool enableDistributedCkpt) {

    unsigned count = 0;

    // Insert boundary checks
    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);
        if (boundaries.count(blockName)) {
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

} // namespace checkpoint
