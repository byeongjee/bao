#include "rockclimb/RockClimbInstrumenter.h"
#include "common/BlockUtils.h"

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

// === Memory checkpointing implementation ===

llvm::BasicBlock* RockClimbInstrumenter::getBlockByName(
    llvm::Function &F,
    const std::string &name) {

    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);
        if (blockName == name) {
            return &BB;
        }
    }
    return nullptr;
}

llvm::CallInst* RockClimbInstrumenter::findRockClimbCheck(llvm::BasicBlock &BB) {
    for (llvm::Instruction &I : BB) {
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&I)) {
            llvm::Function *callee = call->getCalledFunction();
            if (callee && callee->getName() == "__rockclimb_check") {
                return call;
            }
        }
    }
    return nullptr;
}

llvm::GlobalVariable* RockClimbInstrumenter::getOrCreateNVMSlot(
    const MemoryCheckpointPoint &ckpt) {

    // Check if already exists
    auto it = nvmSlots_.find(ckpt.nvmSlotName);
    if (it != nvmSlots_.end()) {
        return it->second;
    }

    llvm::LLVMContext &Ctx = M_.getContext();

    // Create NVM global variable for this slot
    // Type matches the original memory location's type
    llvm::GlobalVariable *nvmSlot = new llvm::GlobalVariable(
        M_,
        ckpt.valueType,
        false,  // not constant
        llvm::GlobalValue::InternalLinkage,
        llvm::Constant::getNullValue(ckpt.valueType),
        ckpt.nvmSlotName
    );

    // Set section to ".nvm" for NVM placement
    nvmSlot->setSection(".nvm");

    nvmSlots_[ckpt.nvmSlotName] = nvmSlot;
    return nvmSlot;
}

void RockClimbInstrumenter::insertMemoryToNVMStore(
    llvm::IRBuilder<> &Builder,
    const MemoryCheckpointPoint &ckpt) {

    // Get or create the NVM slot
    llvm::GlobalVariable *nvmSlot = getOrCreateNVMSlot(ckpt);

    // Load from original memory location
    llvm::Value *val = Builder.CreateLoad(ckpt.valueType, ckpt.memLoc,
                                           ckpt.nvmSlotName + ".load");

    // Store to NVM slot
    Builder.CreateStore(val, nvmSlot);
}

unsigned RockClimbInstrumenter::instrumentMemoryCheckpoints(
    llvm::Function &F,
    const MemoryCheckpointResult &memCkpts,
    const std::vector<std::string> &boundaries) {

    llvm::LLVMContext &Ctx = M_.getContext();
    unsigned count = 0;

    // Ensure runtime symbols are declared
    declareRuntimeSymbols();

    // Store checkpoint info for recovery dispatcher
    memCkptsByBoundary_ = memCkpts.byBoundary;

    // For each boundary with memory checkpoints
    for (const auto &[boundaryId, ckptList] : memCkpts.byBoundary) {
        // Skip if no checkpoints for this boundary
        if (ckptList.empty()) continue;

        // Find the boundary block (boundary ID corresponds to region index,
        // and the actual boundary block is the start of the next region)
        // The boundaries vector is indexed differently - need to map correctly
        if (boundaryId >= boundaries.size()) continue;

        std::string boundaryBlockName = boundaries[boundaryId];
        llvm::BasicBlock *BB = getBlockByName(F, boundaryBlockName);
        if (!BB) continue;

        // Find the __rockclimb_check() call in this block
        llvm::CallInst *checkCall = findRockClimbCheck(*BB);
        if (!checkCall) {
            // If no check call yet, insert after PHIs and allocas
            llvm::BasicBlock::iterator insertPt = BB->getFirstNonPHIIt();
            // Skip past allocas to avoid breaking alloca dominance
            while (insertPt != BB->end() && llvm::isa<llvm::AllocaInst>(&*insertPt)) {
                ++insertPt;
            }
            if (insertPt == BB->end()) {
                // Block only has PHIs/allocas and terminator, insert before terminator
                insertPt = std::prev(BB->end());
            }
            llvm::IRBuilder<> Builder(&*insertPt);

            // Save each live memory location
            for (const auto &ckpt : ckptList) {
                insertMemoryToNVMStore(Builder, ckpt);
                ++count;
            }

            // Store boundary ID for recovery
            llvm::Value *boundaryIdVal = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(Ctx), boundaryId + 1);  // +1 since 0 means "no recovery needed"
            Builder.CreateStore(boundaryIdVal, nvmRegionId_);
        } else {
            // Insert BEFORE the check call
            llvm::IRBuilder<> Builder(checkCall);

            // Save each live memory location
            for (const auto &ckpt : ckptList) {
                insertMemoryToNVMStore(Builder, ckpt);
                ++count;
            }

            // Store boundary ID for recovery
            llvm::Value *boundaryIdVal = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(Ctx), boundaryId + 1);
            Builder.CreateStore(boundaryIdVal, nvmRegionId_);
        }
    }

    return count;
}

llvm::Function* RockClimbInstrumenter::generateRestoreFunction(
    unsigned boundaryId,
    const std::vector<MemoryCheckpointPoint> &ckpts) {

    llvm::LLVMContext &Ctx = M_.getContext();

    // Create function: void __restore_boundary_N(void)
    std::string fnName = "__restore_boundary_" + std::to_string(boundaryId);

    // Check if already exists
    if (llvm::Function *existing = M_.getFunction(fnName)) {
        return existing;
    }

    llvm::FunctionType *FT = llvm::FunctionType::get(
        llvm::Type::getVoidTy(Ctx), false);
    llvm::Function *RestoreFn = llvm::Function::Create(
        FT, llvm::GlobalValue::InternalLinkage, fnName, M_);

    llvm::BasicBlock *Entry = llvm::BasicBlock::Create(Ctx, "entry", RestoreFn);
    llvm::IRBuilder<> Builder(Entry);

    // For each checkpointed memory location
    for (const auto &ckpt : ckpts) {
        // Get the NVM slot (should already exist from instrumentMemoryCheckpoints)
        auto it = nvmSlots_.find(ckpt.nvmSlotName);
        if (it == nvmSlots_.end()) {
            // Create it if it doesn't exist (defensive)
            getOrCreateNVMSlot(ckpt);
            it = nvmSlots_.find(ckpt.nvmSlotName);
        }
        llvm::GlobalVariable *nvmSlot = it->second;

        // Load from NVM slot
        llvm::Value *val = Builder.CreateLoad(ckpt.valueType, nvmSlot,
                                               ckpt.nvmSlotName + ".restore");

        // Store back to original memory location
        Builder.CreateStore(val, ckpt.memLoc);
    }

    Builder.CreateRetVoid();
    return RestoreFn;
}

void RockClimbInstrumenter::insertRecoveryDispatcher(
    llvm::Function &F,
    const std::map<unsigned, llvm::Function*> &restoreFns,
    const std::map<unsigned, llvm::BasicBlock*> &boundaryBlocks) {

    // Note: restoreFns is kept for API compatibility but not used anymore.
    // We now inline the restore logic directly using memCkptsByBoundary_.
    (void)restoreFns;  // Suppress unused warning

    if (boundaryBlocks.empty() && memCkptsByBoundary_.empty()) return;

    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::BasicBlock &Entry = F.getEntryBlock();

    // Ensure runtime symbols are declared
    declareRuntimeSymbols();

    // Find first non-alloca instruction to split at
    llvm::BasicBlock::iterator SplitPt = Entry.begin();
    while (SplitPt != Entry.end() && llvm::isa<llvm::AllocaInst>(&*SplitPt)) {
        ++SplitPt;
    }

    // If all instructions are allocas (or block is empty), insert at end
    if (SplitPt == Entry.end()) {
        // Can't split here, just return
        return;
    }

    // Split entry block after allocas
    llvm::BasicBlock *OrigEntry = Entry.splitBasicBlock(SplitPt, "orig_entry");

    // Remove the unconditional branch that splitBasicBlock created
    Entry.getTerminator()->eraseFromParent();

    llvm::IRBuilder<> Builder(&Entry);

    // Load region_id
    llvm::Value *regionId = Builder.CreateLoad(
        llvm::Type::getInt32Ty(Ctx), nvmRegionId_, "region_id");

    // Check if recovery needed (region_id != 0)
    llvm::Value *needsRecovery = Builder.CreateICmpNE(
        regionId, llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0),
        "needs_recovery");

    // Create recovery block
    llvm::BasicBlock *RecoveryBB = llvm::BasicBlock::Create(
        Ctx, "recovery", &F, OrigEntry);
    Builder.CreateCondBr(needsRecovery, RecoveryBB, OrigEntry);

    // Build recovery switch
    Builder.SetInsertPoint(RecoveryBB);

    // Collect all boundary IDs from both maps
    std::set<unsigned> allBoundaryIds;
    for (const auto &[id, _] : boundaryBlocks) {
        allBoundaryIds.insert(id);
    }
    for (const auto &[id, _] : memCkptsByBoundary_) {
        allBoundaryIds.insert(id);
    }

    llvm::SwitchInst *Switch = Builder.CreateSwitch(regionId, OrigEntry,
                                                     allBoundaryIds.size());

    for (unsigned boundaryId : allBoundaryIds) {
        // Find target block for this boundary
        auto targetIt = boundaryBlocks.find(boundaryId);
        llvm::BasicBlock *targetBB = (targetIt != boundaryBlocks.end())
            ? targetIt->second : OrigEntry;

        // Skip boundary 0 if it's the entry block - can't jump back to entry
        // Entry block can't have predecessors in LLVM IR
        if (targetBB == &Entry || targetBB->isEntryBlock()) {
            // For entry boundary, just go to OrigEntry (normal start)
            targetBB = OrigEntry;
        }

        // Create case block for this boundary
        llvm::BasicBlock *CaseBB = llvm::BasicBlock::Create(
            Ctx, "restore_" + std::to_string(boundaryId), &F);

        llvm::IRBuilder<> CaseBuilder(CaseBB);

        // Inline restore logic: load from NVM slots, store to original locations
        auto ckptIt = memCkptsByBoundary_.find(boundaryId);
        if (ckptIt != memCkptsByBoundary_.end()) {
            for (const auto &ckpt : ckptIt->second) {
                // Get the NVM slot
                auto slotIt = nvmSlots_.find(ckpt.nvmSlotName);
                if (slotIt != nvmSlots_.end()) {
                    llvm::GlobalVariable *nvmSlot = slotIt->second;

                    // Load from NVM slot
                    llvm::Value *val = CaseBuilder.CreateLoad(
                        ckpt.valueType, nvmSlot,
                        ckpt.nvmSlotName + ".restore");

                    // Store to original memory location
                    CaseBuilder.CreateStore(val, ckpt.memLoc);
                }
            }
        }

        // Clear region_id after restore
        CaseBuilder.CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), 0), nvmRegionId_);

        // Jump to the target block
        CaseBuilder.CreateBr(targetBB);

        // Add case to switch (boundary IDs are stored as boundaryId + 1)
        Switch->addCase(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(Ctx), boundaryId + 1),
            CaseBB);
    }
}

} // namespace checkpoint
