#include "milp/CheckpointInstrumenter.h"
#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/ValueOrder.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

#include <cassert>
#include <vector>

namespace checkpoint {

CheckpointInstrumenter::CheckpointInstrumenter(llvm::Module &M) : M_(M) {
    declareRuntimeFunctions();
}

void CheckpointInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);

    // Declared for linkage visibility; boundary calls are emitted as inline
    // asm (with full GPR clobbers) rather than through this callee.
    M_.getOrInsertFunction("__region_boundary", VoidTy);

    // NOTE: Per-operation debug counter increments (cnt_save_vreg,
    // cnt_restore_vreg, cnt_store_mem, cnt_restore_mem) are not emitted:
    // each increment inserts a load-add-store sequence per
    // save/restore/commit point, which overflows FRAM on large benchmarks
    // (e.g., RSA overflows by ~38KB). Only cnt_boundary (incremented in
    // boot.S assembly) is kept.
}

unsigned CheckpointInstrumenter::instrumentFunction(llvm::Function &F, const MILPSolution &solution,
                                                    const ICFGView &cfg,
                                                    const IStateView &stateView,
                                                    const StateAnalysis &state) {

    applyMemoryPlacement(state);
    createShadowGlobals(F, solution, state);
    createNVMBackupGlobals(F, solution, state);
    rewriteAccessesInVMRegions(F, solution, cfg);
    unsigned inserted = insertRegionBoundaries(F, solution, cfg, stateView, state);
    return inserted;
}

void CheckpointInstrumenter::applyMemoryPlacement(const StateAnalysis &state) {
    // Ensure VM-placed globals are in FRAM (non-volatile).
    // Globals already annotated (e.g., .fram) keep their section.
    // Unannotated globals get placed in .fram explicitly.
    for (llvm::GlobalVariable *GV : state.getVMObjs()) {
        if (GV->getSection().empty())
            GV->setSection(".fram");
    }
}

void CheckpointInstrumenter::createShadowGlobals(llvm::Function &F, const MILPSolution &solution,
                                                 const StateAnalysis &state) {

    shadowMap_.clear();

    // Collect candidate GVs that have placeInVm=true for at least one node.
    std::vector<llvm::GlobalVariable *> vmPlacedGVs;
    for (const auto &[key, placed] : solution.m) {
        if (!placed)
            continue;
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(key.second))
            vmPlacedGVs.push_back(GV);
    }
    stableSortAndUniqueValues(vmPlacedGVs);

    for (llvm::GlobalVariable *GV : vmPlacedGVs) {
        auto *shadow = new llvm::GlobalVariable(
            M_, GV->getValueType(), /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(GV->getValueType()), "__vm_shadow_" + GV->getName().str());
        shadow->setAlignment(GV->getAlign());
        shadowMap_[GV] = shadow;
    }
}

void CheckpointInstrumenter::createNVMBackupGlobals(llvm::Function &F, const MILPSolution &solution,
                                                    const StateAnalysis &state) {

    nvmBackupMap_.clear();
    allocaAddrMap_.clear();

    unsigned ssaCounter = 0;
    for (llvm::Value *V : state.getIneligibleObjs()) {
        llvm::Type *backupType = nullptr;
        std::string backupName;

        if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V)) {
            auto *arraySizeCI = llvm::cast<llvm::ConstantInt>(AI->getArraySize());
            uint64_t elemCount = arraySizeCI->getZExtValue();
            if (elemCount <= 1) {
                backupType = AI->getAllocatedType();
            } else {
                backupType = llvm::ArrayType::get(AI->getAllocatedType(), elemCount);
            }
            backupName = "__nvm_alloca_" +
                         (AI->hasName() ? AI->getName().str() : std::to_string(ssaCounter++));
        } else if (auto *Inst = llvm::dyn_cast<llvm::Instruction>(V)) {
            backupType = Inst->getType();
            backupName = "__nvm_ssa_" + std::to_string(ssaCounter++);
        } else {
            // Only allocas and SSA instructions are supported here. Anything
            // else (e.g. a GlobalVariable) has no def tracking or backup
            // emission path — silently skipping it would drop its state
            // across region boundaries.
            llvm::report_fatal_error(llvm::Twine("MILP instrumenter: unsupported ineligible "
                                                 "object kind for '") +
                                         V->getName() + "'",
                                     /*gen_crash_diag=*/false);
        }

        auto *backup = new llvm::GlobalVariable(
            M_, backupType, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(backupType), backupName);
        backup->setSection(".nvm");
        nvmBackupMap_[V] = backup;

        // For allocas, also create an NVM slot holding the alloca's address.
        // Restore code after a boundary must not reference the alloca
        // directly: llc would keep the SP-relative address in a register or
        // stack spill slot across the boundary, both of which a BOR reboot
        // destroys. Instead the address is stored to FRAM before the
        // boundary and reloaded after it (the stack layout is deterministic
        // across recovery, so the stored address stays valid).
        if (llvm::isa<llvm::AllocaInst>(V)) {
            llvm::Type *ptrTy = llvm::PointerType::getUnqual(M_.getContext());
            auto *addrSlot = new llvm::GlobalVariable(
                M_, ptrTy, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(ptrTy), backupName + "_addr");
            addrSlot->setSection(".nvm");
            allocaAddrMap_[V] = addrSlot;
        }
    }
}

void CheckpointInstrumenter::rewriteAccessesInVMRegions(llvm::Function &F,
                                                        const MILPSolution &solution,
                                                        const ICFGView &cfg) {

    // Redirect memory ACCESSES (not raw operands) in m=1 blocks to the VM
    // shadow. Rewriting operands block-locally is unsound: a pointer derived
    // from the global in an m=1 block (e.g. a LICM-hoisted GEP) can flow
    // into m=0 blocks, whose accesses would then silently hit the shadow
    // while the MILP model assumes they hit FRAM — writes end up in SRAM
    // with no commit modeled, and a BOR reboot discards them. Instead, each
    // access in an m=1 block recomputes its address as
    //   shadow + (ptr - global)
    // so an access follows its own block's placement no matter where its
    // pointer operand was computed.
    //
    // Bases are resolved with resolveUniqueUnderlyingGlobal, which looks
    // through phi/select: shadow + (ptr - global) is correct for any runtime
    // pointer value as long as every possible base is the same global (e.g.
    // a pointer induction variable over a global array). Pointers with more
    // than one possible base never reach here — StateAnalysis strict mode
    // rejects the whole function for those.
    const NodeMap &nodeMap = cfg.getNodeMap();
    const llvm::DataLayout &DL = M_.getDataLayout();
    llvm::Type *intPtrTy = DL.getIntPtrType(M_.getContext());
    llvm::Type *i8Ty = llvm::Type::getInt8Ty(M_.getContext());

    auto shadowForUnderlying =
        [&](llvm::Value *Ptr) -> std::pair<llvm::GlobalVariable *, llvm::GlobalVariable *> {
        llvm::GlobalVariable *GV = resolveUniqueUnderlyingGlobal(Ptr);
        if (!GV)
            return {nullptr, nullptr};
        auto it = shadowMap_.find(GV);
        if (it == shadowMap_.end())
            return {nullptr, nullptr};
        return {GV, it->second};
    };

    for (llvm::BasicBlock &BB : F) {
        NodeId nodeId = nodeMap.getNodeId(&BB);
        if (nodeId == kInvalidNodeId)
            continue;

        auto placedInVm = [&](llvm::GlobalVariable *GV) {
            auto pvIt = solution.m.find(std::make_pair(nodeId, static_cast<llvm::Value *>(GV)));
            return pvIt != solution.m.end() && pvIt->second;
        };

        auto redirect = [&](llvm::Instruction &I, unsigned opIdx) {
            llvm::Value *Ptr = I.getOperand(opIdx);
            auto [GV, shadow] = shadowForUnderlying(Ptr);
            if (!GV || !placedInVm(GV))
                return;
            llvm::IRBuilder<> builder(&I);
            llvm::Value *newPtr = shadow;
            if (Ptr != GV) {
                llvm::Value *off =
                    builder.CreateSub(builder.CreatePtrToInt(Ptr, intPtrTy),
                                      builder.CreatePtrToInt(GV, intPtrTy), "vm.off");
                newPtr = builder.CreateGEP(i8Ty, shadow, off, "vm.addr");
            }
            I.setOperand(opIdx, newPtr);
        };

        for (llvm::Instruction &I : llvm::make_early_inc_range(BB)) {
            if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                redirect(I, LI->getPointerOperandIndex());
            } else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                redirect(I, SI->getPointerOperandIndex());
            } else if (auto *MI = llvm::dyn_cast<llvm::MemIntrinsic>(&I)) {
                redirect(I, 0); // dest
                if (llvm::isa<llvm::MemTransferInst>(MI))
                    redirect(I, 1); // source
            }
        }
    }
}

unsigned CheckpointInstrumenter::insertRegionBoundaries(llvm::Function &F,
                                                        const MILPSolution &solution,
                                                        const ICFGView &cfg,
                                                        const IStateView &stateView,
                                                        const StateAnalysis &state) {

    unsigned inserted = 0;
    NodeId entryNode = cfg.getEntryBlock();

    llvm::DenseMap<llvm::BasicBlock *, std::vector<NodeId>> nodesByBlock;
    for (NodeId node : solution.r) {
        llvm::BasicBlock *BB = cfg.getNodeMap().getConcreteBlock(node);
        if (!BB || BB->getParent() != &F) {
            PLOGW << "CheckpointInstrumenter: unresolved region-start node "
                  << cfg.getNodeName(node);
            continue;
        }
        nodesByBlock[BB].push_back(node);
    }

    // Accumulated across all boundary blocks for SSAUpdater-based restore
    // rewriting (all SSA values, unified path).
    std::map<llvm::Value *, std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>>>
        ssaRestoreDefs;

    for (llvm::BasicBlock &BB : F) {
        auto itNodeVec = nodesByBlock.find(&BB);
        if (itNodeVec == nodesByBlock.end())
            continue;

        const std::vector<NodeId> &nodes = itNodeVec->second;
        std::set<NodeId> nodeSet(nodes.begin(), nodes.end());

        llvm::BasicBlock::iterator insertIt = checkpoint::getInsertPointAfterAllocas(BB);
        if (insertIt == BB.end())
            continue;

        llvm::IRBuilder<> builder(&BB, insertIt);

        bool isEntryNode = nodeSet.count(entryNode) > 0;

        // For b != b0, emit: commit stores -> region boundary -> split -> restores
        if (!isEntryNode) {
            // --- Commit phase (all in BB, before boundary) ---

            std::vector<llvm::Value *> commitVars;
            for (NodeId n : nodeSet)
                for (llvm::Value *V : solution.getSaveVarsAt(n))
                    commitVars.push_back(V);

            // All ineligible live-in values are committed here regardless of
            // the solver's save decisions: the restore phase below reloads
            // every ineligible live-in from its NVM backup unconditionally,
            // so each one must have a current backup before the boundary.
            for (llvm::Value *V : state.getIneligLiveIn(&BB))
                commitVars.push_back(V);
            stableSortAndUniqueValues(commitVars);

            for (llvm::Value *V : commitVars) {
                unsigned sizeBytes = state.getVarSizeBytes(V);
                if (sizeBytes == 0)
                    continue;

                if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
                    llvm::Value *size =
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                    auto shadowIt = shadowMap_.find(GV);
                    assert(shadowIt != shadowMap_.end() &&
                           "eligible global commit requires a VM shadow");
                    builder.CreateMemCpy(GV, GV->getAlign(), shadowIt->second,
                                         shadowIt->second->getAlign(), size);
                } else if (llvm::isa<llvm::AllocaInst>(V)) {
                    llvm::Value *size =
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() &&
                           "alloca commit requires an NVM backup");
                    auto *AI = llvm::cast<llvm::AllocaInst>(V);
                    builder.CreateMemCpy(backupIt->second, backupIt->second->getAlign(), AI,
                                         AI->getAlign(), size);
                    // Persist the alloca's address so the restore code after
                    // the boundary can reload it from FRAM instead of keeping
                    // it live (in a register or spill slot) across the reboot.
                    auto addrIt = allocaAddrMap_.find(V);
                    assert(addrIt != allocaAddrMap_.end() && "alloca commit requires an addr slot");
                    builder.CreateStore(AI, addrIt->second);
                } else {
                    // Invariant: the solver only orders saves of values that
                    // are live-in at their node, and a node's entry is the
                    // top of its representative block — the exact point where
                    // this boundary is emitted. A violation means the model
                    // accounted a different boundary point than the one being
                    // materialized.
                    assert(state.getIneligLiveIn(&BB).count(V) &&
                           "solver-ordered save must be live-in at the boundary");
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() && "SSA commit requires an NVM backup");
                    builder.CreateStore(V, backupIt->second);
                }
                inserted++;
            }

            // Emit boundary call as inline asm that clobbers all GPRs and
            // memory. A BOR reboot at the boundary destroys every register
            // except SP (restored by recovery), so the call must not follow
            // the C ABI's callee-saved contract: values kept in R4-R10
            // across the call would silently read back as garbage after
            // recovery. The ~{memory} clobber also stops llc from
            // forwarding pre-boundary values (e.g. the FRAM addr-slot
            // stores) across the reboot.
            llvm::FunctionType *asmTy =
                llvm::FunctionType::get(llvm::Type::getVoidTy(M_.getContext()), false);
            llvm::InlineAsm *boundaryAsm = llvm::InlineAsm::get(
                asmTy, "call #__region_boundary",
                "~{r4},~{r5},~{r6},~{r7},~{r8},~{r9},~{r10},~{r11},~{r12},~{r13},~{r14},~{r15},"
                "~{memory}",
                /*hasSideEffects=*/true);

            // --- Split block and emit the boundary as a callbr terminator ---
            // BB_bottom receives the rest of the block; restores go there.
            // A boundary is a resume point: after power loss, execution
            // restarts at BB_bottom with only FRAM intact, so everything
            // from there on must be recomputable from FRAM alone. Emitting
            // the boundary as a terminator makes that structural: no
            // instruction can sit between it and the block end, so codegen
            // can never place post-resume work ahead of the resume point.
            llvm::BasicBlock *BB_bottom = llvm::SplitBlock(&BB, &*builder.GetInsertPoint());
            BB.getTerminator()->eraseFromParent();
            builder.SetInsertPoint(&BB);
            builder.CreateCallBr(asmTy, boundaryAsm, BB_bottom, {}, {});
            inserted++;
            builder.SetInsertPoint(&*BB_bottom->getFirstInsertionPt());
        }
        // Entry node: no boundary call, no split. Restores still emitted below.

        // --- Restore phase (in BB_bottom for non-entry, in BB for entry) ---
        // The builder is now positioned in the restore block.

        // Eligible restores (needRestore).
        std::vector<llvm::GlobalVariable *> restoreGVs;
        for (NodeId n : nodeSet) {
            for (llvm::Value *V : solution.getRestoreVarsAt(n)) {
                if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
                    restoreGVs.push_back(GV);
            }
        }
        stableSortAndUniqueValues(restoreGVs);

        for (llvm::GlobalVariable *GV : restoreGVs) {
            unsigned sizeBytes = state.getVarSizeBytes(GV);
            if (sizeBytes == 0)
                continue;
            llvm::Value *size =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
            auto shadowIt = shadowMap_.find(GV);
            assert(shadowIt != shadowMap_.end() &&
                   "needRestore requires a VM shadow for the global");
            builder.CreateMemCpy(shadowIt->second, shadowIt->second->getAlign(), GV, GV->getAlign(),
                                 size);
            inserted++;
        }

        // Ineligible restores: unconditional at every region start where
        // the object is live-in.
        std::vector<llvm::Value *> orderedIneligLiveIn(state.getIneligLiveIn(&BB).begin(),
                                                       state.getIneligLiveIn(&BB).end());
        stableSortAndUniqueValues(orderedIneligLiveIn);
        for (llvm::Value *V : orderedIneligLiveIn) {
            auto backupIt = nvmBackupMap_.find(V);
            if (backupIt == nvmBackupMap_.end())
                continue;

            if (llvm::isa<llvm::AllocaInst>(V)) {
                unsigned sizeBytes = state.getVarSizeBytes(V);
                if (sizeBytes == 0)
                    continue;
                llvm::Value *size =
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                auto *AI = llvm::cast<llvm::AllocaInst>(V);
                // After a boundary, the alloca's address must come from the
                // FRAM addr slot (committed just before the boundary): any
                // register or spill slot holding it is destroyed by the BOR
                // reboot. At the entry node there is no reboot in between,
                // so the alloca can be referenced directly.
                llvm::Value *dst = AI;
                if (!isEntryNode) {
                    auto addrIt = allocaAddrMap_.find(V);
                    assert(addrIt != allocaAddrMap_.end() &&
                           "alloca restore requires an addr slot");
                    dst = builder.CreateLoad(llvm::PointerType::getUnqual(M_.getContext()),
                                             addrIt->second);
                }
                builder.CreateMemCpy(dst, AI->getAlign(), backupIt->second,
                                     backupIt->second->getAlign(), size);
                inserted++;
            } else {
                // SSA restore: load from NVM backup, record for SSAUpdater.
                llvm::Value *restored = builder.CreateLoad(V->getType(), backupIt->second);
                ssaRestoreDefs[V].emplace_back(builder.GetInsertBlock(), restored);
                inserted++;
            }
        }
    }

    // SSAUpdater pass: rewrite uses of SSA values to pick up restore loads.
    // Each SSA value V has available definitions:
    //   - V itself in its def block (original definition)
    //   - restored value in each BB_bottom (after boundary)
    // SSAUpdater selects the correct reaching definition for each use.
    std::vector<llvm::Value *> orderedSSAValues;
    orderedSSAValues.reserve(ssaRestoreDefs.size());
    for (const auto &[origVal, defs] : ssaRestoreDefs)
        orderedSSAValues.push_back(origVal);
    stableSortAndUniqueValues(orderedSSAValues);

    for (llvm::Value *origVal : orderedSSAValues) {
        auto &defs = ssaRestoreDefs[origVal];
        llvm::SSAUpdater updater;
        updater.Initialize(origVal->getType(), origVal->getName());

        auto *defInst = llvm::cast<llvm::Instruction>(origVal);
        updater.AddAvailableValue(defInst->getParent(), origVal);

        for (auto &[block, restoredVal] : defs)
            updater.AddAvailableValue(block, restoredVal);

        llvm::SmallVector<llvm::Use *, 16> usesToRewrite;
        for (auto &U : origVal->uses()) {
            auto *I = llvm::dyn_cast<llvm::Instruction>(U.getUser());
            if (!I)
                continue;
            // For PHI nodes, the "effective" block of the use is the incoming
            // block, not the block containing the PHI.
            llvm::BasicBlock *useBlock;
            if (auto *PN = llvm::dyn_cast<llvm::PHINode>(I))
                useBlock = PN->getIncomingBlock(U);
            else
                useBlock = I->getParent();
            // Skip uses in the def block — they always see the original value.
            if (useBlock == defInst->getParent())
                continue;
            usesToRewrite.push_back(&U);
        }

        for (llvm::Use *U : usesToRewrite)
            updater.RewriteUseAfterInsertions(*U);
    }

    return inserted;
}

} // namespace checkpoint
