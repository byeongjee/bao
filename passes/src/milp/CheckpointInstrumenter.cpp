#include "milp/CheckpointInstrumenter.h"
#include "common/BlockUtils.h"

#include "llvm/IR/IRBuilder.h"

namespace checkpoint {

CheckpointInstrumenter::CheckpointInstrumenter(llvm::Module &M) : M_(M) {
    declareRuntimeFunctions();
}

void CheckpointInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
    llvm::Type *I64Ty = llvm::Type::getInt64Ty(Ctx);
    llvm::Type *PtrTy = llvm::PointerType::get(Ctx, 0);

    // void __region_prologue(void)
    prologueFn_ = M_.getOrInsertFunction("__region_prologue", VoidTy);

    // void __region_epilogue(void)
    epilogueFn_ = M_.getOrInsertFunction("__region_epilogue", VoidTy);

    // void __checkpoint_store_reg(i32 slot_id, i64 value)
    storeRegFn_ = M_.getOrInsertFunction("__checkpoint_store_reg",
                                          VoidTy, I32Ty, I64Ty);

    // void __checkpoint_store_mem(ptr nvm_dst, ptr vm_src, i32 size)
    storeMemFn_ = M_.getOrInsertFunction("__checkpoint_store_mem",
                                          VoidTy, PtrTy, PtrTy, I32Ty);

    // void __restore_reg(i32 slot_id, ptr dest)
    restoreRegFn_ = M_.getOrInsertFunction("__restore_reg",
                                            VoidTy, I32Ty, PtrTy);

    // void __restore_mem(ptr vm_dst, ptr nvm_src, i32 size)
    restoreMemFn_ = M_.getOrInsertFunction("__restore_mem",
                                            VoidTy, PtrTy, PtrTy, I32Ty);
}

unsigned CheckpointInstrumenter::instrumentFunction(
    llvm::Function &F,
    const MILPSolution &solution,
    const StateAnalysis &state) {

    unsigned count = 0;

    // Insert region boundaries (prologue/epilogue)
    insertRegionBoundaries(F, solution, state);

    // Insert distributed checkpoint stores
    insertDistributedStores(F, solution, state);

    // Apply memory placement to globals
    applyMemoryPlacement(solution);

    // Count instrumentation points
    count = solution.regionStarts.size() + solution.enabledDefStores.size();
    return count;
}

void CheckpointInstrumenter::insertRegionBoundaries(
    llvm::Function &F,
    const MILPSolution &solution,
    const StateAnalysis &state) {

    llvm::LLVMContext &Ctx = M_.getContext();

    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);

        if (!solution.regionStarts.count(blockName))
            continue;

        // Insert epilogue at the end of each predecessor
        for (llvm::BasicBlock *Pred : llvm::predecessors(&BB)) {
            // Insert before the terminator of the predecessor
            llvm::Instruction *Term = Pred->getTerminator();
            if (Term) {
                llvm::IRBuilder<> Builder(Term);
                Builder.CreateCall(epilogueFn_);
            }
        }

        // Insert prologue at the beginning of this block (after PHIs)
        llvm::BasicBlock::iterator InsertPt = BB.getFirstNonPHIIt();
        llvm::IRBuilder<> Builder(&*InsertPt);

        // Prologue call
        Builder.CreateCall(prologueFn_);

        // Restore SSA regs that are live-in
        const auto &regLiveIn = state.getRegLiveIn(blockName);
        for (llvm::Value *V : regLiveIn) {
            int elemId = state.getRegStateElemId(V);
            if (elemId < 0)
                continue;

            // Create an alloca to receive the restored value, then load it
            // The restore_reg function writes the value to the provided pointer
            llvm::Value *SlotId = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(Ctx), static_cast<unsigned>(elemId));

            // Create alloca in entry block for the restore target
            llvm::IRBuilder<> AllocaBuilder(
                &F.getEntryBlock(), F.getEntryBlock().begin());
            llvm::AllocaInst *RestoreSlot = AllocaBuilder.CreateAlloca(
                V->getType(), nullptr, "restore_slot");

            Builder.CreateCall(restoreRegFn_, {SlotId, RestoreSlot});
        }

        // Restore VMObjs that need volatile restore
        const auto &vmObjLiveIn = state.getVMObjLiveIn(blockName);
        for (llvm::GlobalVariable *GV : vmObjLiveIn) {
            int elemId = state.getVMObjStateElemId(GV);
            if (elemId < 0)
                continue;

            auto key =
                std::make_pair(blockName, static_cast<unsigned>(elemId));
            auto it = solution.needVolRestore.find(key);
            if (it == solution.needVolRestore.end() || !it->second)
                continue;

            const StateElement &elem =
                state.getStateElement(static_cast<unsigned>(elemId));

            // __restore_mem(vm_dst=GV, nvm_src=nvm_shadow, size)
            // The NVM shadow address is determined by the runtime; we pass the
            // global's address as both source and destination (the runtime maps
            // the NVM shadow).
            llvm::Value *Size = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(Ctx), elem.sizeBytes);
            Builder.CreateCall(restoreMemFn_, {GV, GV, Size});
        }
    }
}

void CheckpointInstrumenter::insertDistributedStores(
    llvm::Function &F,
    const MILPSolution &solution,
    const StateAnalysis &state) {

    llvm::LLVMContext &Ctx = M_.getContext();

    for (unsigned dsId : solution.enabledDefStores) {
        const DefSite &ds = state.getDefSites()[dsId];
        llvm::Instruction *DefInst = ds.inst;

        // Insert store after the defining instruction
        llvm::BasicBlock::iterator InsertPt(DefInst);
        ++InsertPt;
        // Skip past any PHIs if somehow we're at a weird position
        while (InsertPt != DefInst->getParent()->end() &&
               llvm::isa<llvm::PHINode>(&*InsertPt)) {
            ++InsertPt;
        }
        llvm::IRBuilder<> Builder(&*InsertPt);

        if (ds.kind == DefSite::SSAReg) {
            // __checkpoint_store_reg(slot_id, value)
            int elemId = state.getRegStateElemId(ds.ssaValue);
            if (elemId < 0)
                continue;

            llvm::Value *SlotId = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(Ctx), static_cast<unsigned>(elemId));

            // Cast value to i64 for the store function
            llvm::Value *Val = ds.ssaValue;
            if (Val->getType()->isIntegerTy()) {
                Val = Builder.CreateZExtOrTrunc(
                    Val, llvm::Type::getInt64Ty(Ctx));
            } else if (Val->getType()->isFloatingPointTy()) {
                Val = Builder.CreateBitCast(
                    Val, llvm::Type::getInt64Ty(Ctx));
            } else if (Val->getType()->isPointerTy()) {
                Val = Builder.CreatePtrToInt(
                    Val, llvm::Type::getInt64Ty(Ctx));
            } else {
                // For other types, skip (shouldn't happen for typical IR)
                continue;
            }

            Builder.CreateCall(storeRegFn_, {SlotId, Val});
        } else {
            // MemoryDef: __checkpoint_store_mem(nvm_dst, vm_src, size)
            llvm::GlobalVariable *GV = ds.globalVar;
            int elemId = state.getVMObjStateElemId(GV);
            if (elemId < 0)
                continue;

            const StateElement &elem =
                state.getStateElement(static_cast<unsigned>(elemId));
            llvm::Value *Size = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(Ctx), elem.sizeBytes);

            // Pass global address as both src and dst (runtime resolves NVM
            // shadow)
            Builder.CreateCall(storeMemFn_, {GV, GV, Size});
        }
    }
}

void CheckpointInstrumenter::applyMemoryPlacement(
    const MILPSolution &solution) {
    for (const auto &[gv, inVm] : solution.vmPlacement) {
        if (!inVm) {
            // NVM placement: set section to ".nvm"
            gv->setSection(".nvm");
        }
        // VM (SRAM): keep default section (no change needed)
    }
}

} // namespace checkpoint
