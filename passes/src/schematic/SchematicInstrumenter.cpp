#include "schematic/SchematicInstrumenter.h"
#include "common/BlockUtils.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <cassert>
#include <set>

namespace checkpoint {

SchematicInstrumenter::SchematicInstrumenter(llvm::Module &M, bool addDebugMarkers, unsigned N_reg)
    : M_(M), addDebugMarkers_(addDebugMarkers), N_reg_(N_reg) {}

void SchematicInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
    llvm::Type *PtrTy = llvm::PointerType::getUnqual(Ctx);

    prologueFn_ = M_.getOrInsertFunction("__region_prologue", VoidTy);
    epilogueFn_ = M_.getOrInsertFunction("__region_epilogue", VoidTy);

    llvm::Type *I64Ty = llvm::Type::getInt64Ty(Ctx);
    storeMemFn_ = M_.getOrInsertFunction("__checkpoint_store_mem", VoidTy, PtrTy, PtrTy, I32Ty);
    restoreMemFn_ = M_.getOrInsertFunction("__restore_mem", VoidTy, PtrTy, PtrTy, I32Ty);
    storeRegFn_ = M_.getOrInsertFunction("__checkpoint_store_reg", VoidTy, I32Ty, I64Ty);
    restoreRegFn_ = M_.getOrInsertFunction("__restore_reg", VoidTy, I32Ty, PtrTy);
}

void SchematicInstrumenter::createShadowGlobals(llvm::Function &F,
                                                const SchematicSolution &solution,
                                                const SchematicStateAnalysis &state) {

    shadowMap_.clear();

    // Collect all candidates (globals + allocas) that have Placement::VM in any region.
    std::set<llvm::Value *> vmPlacedVals;
    for (const auto &region : solution.regions) {
        for (const auto &[v, place] : region.allocation.placement) {
            if (place == Placement::VM)
                vmPlacedVals.insert(v);
        }
    }
    // Also check loop decisions.
    for (const auto &[header, dec] : solution.loopDecisions) {
        for (const auto &[v, place] : dec.bodyAllocation.placement) {
            if (place == Placement::VM)
                vmPlacedVals.insert(v);
        }
    }

    for (llvm::Value *V : vmPlacedVals) {
        llvm::Type *shadowType = nullptr;
        std::string shadowName;
        llvm::MaybeAlign shadowAlign;

        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
            shadowType = GV->getValueType();
            shadowName = "__vm_shadow_" + GV->getName().str();
            shadowAlign = GV->getAlign();
        } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V)) {
            shadowType = AI->getAllocatedType();
            shadowName = "__vm_shadow_" + (AI->hasName() ? AI->getName().str() : "alloca");
            shadowAlign = AI->getAlign();
        } else {
            continue;
        }

        auto *shadow = new llvm::GlobalVariable(
            M_, shadowType, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(shadowType), shadowName);
        if (shadowAlign)
            shadow->setAlignment(*shadowAlign);
        shadowMap_[V] = shadow;
    }
}

void SchematicInstrumenter::createIneligibleBackups(llvm::Function &F,
                                                    const SchematicStateAnalysis &state) {

    ineligBackupMap_.clear();
    ineligCheckpointObjs_.clear();

    // With SchematicStateAnalysis, ineligible objects are cross-block SSA values
    // only. These are covered by the runtime's register save/restore mechanism
    // (N_reg), so no NVM backup globals are needed.
}

llvm::BasicBlock *SchematicInstrumenter::splitEdge(llvm::BasicBlock *src, llvm::BasicBlock *dst) {
    return llvm::SplitEdge(src, dst);
}

/// Recursively replace occurrences of GV with Replacement inside a Constant.
static llvm::Constant *replaceGVInConstant(llvm::Constant *C, llvm::GlobalVariable *GV,
                                           llvm::GlobalVariable *Replacement) {
    if (C == GV)
        return Replacement;

    auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(C);
    if (!CE)
        return nullptr;

    bool changed = false;
    llvm::SmallVector<llvm::Constant *, 4> newOps;
    for (unsigned i = 0; i < CE->getNumOperands(); ++i) {
        llvm::Constant *op = CE->getOperand(i);
        llvm::Constant *replaced = replaceGVInConstant(op, GV, Replacement);
        if (replaced) {
            newOps.push_back(replaced);
            changed = true;
        } else {
            newOps.push_back(op);
        }
    }

    if (!changed)
        return nullptr;

    return CE->getWithOperands(newOps);
}

void SchematicInstrumenter::rewriteAccessesInRegion(const std::vector<llvm::BasicBlock *> &blocks,
                                                    const RegionAllocation &allocation) {

    for (llvm::BasicBlock *BB : blocks) {
        for (const auto &[V, place] : allocation.placement) {
            if (place != Placement::VM)
                continue;
            auto shadowIt = shadowMap_.find(V);
            if (shadowIt == shadowMap_.end())
                continue;

            llvm::GlobalVariable *shadow = shadowIt->second;

            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
                // Global: replace direct references and constant expressions.
                for (llvm::Instruction &I : *BB) {
                    for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                        if (I.getOperand(i) == GV) {
                            I.setOperand(i, shadow);
                        } else if (auto *C = llvm::dyn_cast<llvm::Constant>(I.getOperand(i))) {
                            if (auto *replaced = replaceGVInConstant(C, GV, shadow))
                                I.setOperand(i, replaced);
                        }
                    }
                }
            } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V)) {
                // Alloca: replace uses of the alloca pointer with shadow global.
                for (llvm::Instruction &I : *BB) {
                    for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                        if (I.getOperand(i) == AI)
                            I.setOperand(i, shadow);
                    }
                }
            }
        }
    }
}

unsigned SchematicInstrumenter::insertCheckpointSequence(llvm::BasicBlock *ckptBB,
                                                         const RegionAllocation *endingAlloc,
                                                         const RegionAllocation *startingAlloc,
                                                         const SchematicStateAnalysis &state) {

    unsigned inserted = 0;
    llvm::IRBuilder<> builder(ckptBB, ckptBB->getFirstInsertionPt());

    // Phase 1: Save ending region's VM vars with live_end=true.
    // For globals: memcpy shadow -> GV (SRAM -> FRAM).
    // For allocas: memcpy shadow -> alloca (SRAM -> FRAM stack).
    if (endingAlloc) {
        for (const auto &[v, place] : endingAlloc->placement) {
            if (place != Placement::VM)
                continue;
            auto flagIt = endingAlloc->livenessFlags.find(v);
            if (flagIt == endingAlloc->livenessFlags.end() || !flagIt->second.second)
                continue; // live_end = false

            auto shadowIt = shadowMap_.find(v);
            if (shadowIt == shadowMap_.end())
                continue;

            unsigned sizeBytes = state.getVarSizeBytes(v);
            llvm::Value *size =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);

            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
                builder.CreateMemCpy(GV, GV->getAlign(), shadowIt->second,
                                     shadowIt->second->getAlign(), size);
                if (addDebugMarkers_)
                    builder.CreateCall(storeMemFn_, {GV, shadowIt->second, size});
            } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                builder.CreateMemCpy(AI, AI->getAlign(), shadowIt->second,
                                     shadowIt->second->getAlign(), size);
                if (addDebugMarkers_)
                    builder.CreateCall(storeMemFn_, {AI, shadowIt->second, size});
            }
            inserted++;
        }
    }

    // Phase 2: Save ineligible objects to NVM backups.
    // Note: with SchematicStateAnalysis, ineligible objects are cross-block SSA
    // values only. SSA values cannot be memcpy'd — they are handled by the
    // runtime's register save/restore mechanism (N_reg covers them).
    // This phase is kept as a placeholder for future extensions.

    // Phase 3: Epilogue + register save markers.
    builder.CreateCall(epilogueFn_);
    inserted++;
    if (addDebugMarkers_) {
        for (unsigned r = 0; r < N_reg_; ++r) {
            auto *slotId = llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), r);
            auto *val = llvm::ConstantInt::get(llvm::Type::getInt64Ty(M_.getContext()), 0);
            builder.CreateCall(storeRegFn_, {slotId, val});
            inserted++;
        }
    }

    // Phase 4: Prologue + register restore markers.
    builder.CreateCall(prologueFn_);
    inserted++;
    if (addDebugMarkers_) {
        for (unsigned r = 0; r < N_reg_; ++r) {
            auto *slotId = llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), r);
            auto *nullPtr =
                llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(M_.getContext()));
            builder.CreateCall(restoreRegFn_, {slotId, nullPtr});
            inserted++;
        }
    }

    // Phase 5: Restore starting region's VM vars with live_start=true.
    // For globals: memcpy GV -> shadow (FRAM -> SRAM).
    // For allocas: memcpy alloca -> shadow (FRAM stack -> SRAM).
    if (startingAlloc) {
        for (const auto &[v, place] : startingAlloc->placement) {
            if (place != Placement::VM)
                continue;
            auto flagIt = startingAlloc->livenessFlags.find(v);
            if (flagIt == startingAlloc->livenessFlags.end() || !flagIt->second.first)
                continue; // live_start = false

            auto shadowIt = shadowMap_.find(v);
            if (shadowIt == shadowMap_.end())
                continue;

            unsigned sizeBytes = state.getVarSizeBytes(v);
            llvm::Value *size =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);

            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(v)) {
                builder.CreateMemCpy(shadowIt->second, shadowIt->second->getAlign(), GV,
                                     GV->getAlign(), size);
                if (addDebugMarkers_)
                    builder.CreateCall(restoreMemFn_, {shadowIt->second, GV, size});
            } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                builder.CreateMemCpy(shadowIt->second, shadowIt->second->getAlign(), AI,
                                     AI->getAlign(), size);
                if (addDebugMarkers_)
                    builder.CreateCall(restoreMemFn_, {shadowIt->second, AI, size});
            }
            inserted++;
        }
    }

    // Phase 6: Restore ineligible objects from NVM backups.
    // Same as Phase 2 — SSA values are covered by register save/restore.

    return inserted;
}

unsigned
SchematicInstrumenter::insertLoopConditionalCheckpoint(llvm::BasicBlock *header,
                                                       const LoopCheckpointDecision &decision,
                                                       const SchematicStateAnalysis &state) {

    unsigned inserted = 0;
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);

    llvm::Loop *L = decision.loop;
    llvm::BasicBlock *latch = L->getLoopLatch();
    if (!latch)
        return 0;

    llvm::BasicBlock *preheader = L->getLoopPreheader();
    if (!preheader)
        return 0;

    unsigned numIt = decision.numIterationsPerCharge;
    if (numIt == 0)
        return 0;

    // In preheader: alloca counter, store 0.
    llvm::IRBuilder<> preBuilder(preheader->getTerminator());
    llvm::AllocaInst *counter = preBuilder.CreateAlloca(I32Ty, nullptr, "schematic_loop_counter");
    preBuilder.CreateStore(llvm::ConstantInt::get(I32Ty, 0), counter);

    // Split the back-edge (latch → header) using SplitEdge.
    // This correctly updates PHI nodes in header, replacing the latch
    // predecessor with the new intermediate block.
    llvm::BasicBlock *checkBB = llvm::SplitEdge(latch, header);
    if (!checkBB)
        return 0;

    // checkBB currently has an unconditional branch to header.
    // Remove it and insert counter check + conditional branch.
    checkBB->getTerminator()->eraseFromParent();
    checkBB->setName("schematic_loop_check");

    llvm::IRBuilder<> checkBuilder(checkBB);
    llvm::Value *counterVal = checkBuilder.CreateLoad(I32Ty, counter);
    // Evaluate the checkpoint condition before increment so counter=0
    // triggers the initial ("0-th") checkpoint.
    llvm::Value *rem = checkBuilder.CreateURem(counterVal, llvm::ConstantInt::get(I32Ty, numIt));
    llvm::Value *cond = checkBuilder.CreateICmpEQ(rem, llvm::ConstantInt::get(I32Ty, 0));
    llvm::Value *incremented = checkBuilder.CreateAdd(counterVal, llvm::ConstantInt::get(I32Ty, 1));
    checkBuilder.CreateStore(incremented, counter);

    // Create checkpoint BB.
    llvm::BasicBlock *ckptBB =
        llvm::BasicBlock::Create(Ctx, "schematic_loop_ckpt", header->getParent(), header);

    // checkBB: if counter % numIt == 0, goto ckptBB, else goto header.
    checkBuilder.CreateCondBr(cond, ckptBB, header);

    // Fill checkpoint BB: full save/restore sequence, then branch to header.
    inserted +=
        insertCheckpointSequence(ckptBB, &decision.bodyAllocation, &decision.bodyAllocation, state);
    llvm::IRBuilder<> ckptBuilder(ckptBB);
    ckptBuilder.CreateBr(header);

    // Update PHI nodes in header for the new ckptBB predecessor.
    // SplitEdge already replaced latch with checkBB in header's PHIs.
    // Now checkBB conditionally branches to header or ckptBB→header,
    // so header has two new predecessors: checkBB (false) and ckptBB (true).
    for (llvm::PHINode &PHI : header->phis()) {
        llvm::Value *checkVal = PHI.getIncomingValueForBlock(checkBB);
        if (checkVal)
            PHI.addIncoming(checkVal, ckptBB);
    }

    return inserted;
}

unsigned SchematicInstrumenter::instrumentFunction(llvm::Function &F,
                                                   const SchematicSolution &solution,
                                                   const SchematicStateAnalysis &state) {

    unsigned inserted = 0;

    // Step 1: Declare runtime functions.
    declareRuntimeFunctions();

    // Step 2: Apply .nvm section to candidate globals (not allocas).
    for (llvm::Value *V : state.getCandidates()) {
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
            GV->setSection(".nvm");
    }

    // Step 3: Create shadow globals.
    createShadowGlobals(F, solution, state);

    // Step 4: Create ineligible backups.
    createIneligibleBackups(F, state);

    // Step 5: Rewrite accesses in regions.
    for (const auto &region : solution.regions)
        rewriteAccessesInRegion(region.blocks, region.allocation);

    // Step 6: Insert entry prologue after allocas in entry block.
    {
        llvm::BasicBlock &entryBB = F.getEntryBlock();
        llvm::BasicBlock::iterator insertPt = getInsertPointAfterAllocas(entryBB);
        llvm::IRBuilder<> builder(&entryBB, insertPt);
        builder.CreateCall(prologueFn_);
        inserted++;
    }

    // Step 7: Insert checkpoints at enabled edges.
    // Build a lookup from regions for ending/starting allocations.
    // Map each block to its region allocation.
    llvm::DenseMap<llvm::BasicBlock *, const RegionAllocation *> blockToAlloc;
    for (const auto &region : solution.regions) {
        for (llvm::BasicBlock *BB : region.blocks)
            blockToAlloc[BB] = &region.allocation;
    }

    for (const CFGEdge &edge : solution.enabledCheckpoints) {
        // Skip only the true loop back-edge (latch -> header) when loop
        // checkpoint logic (mandatory or conditional) handles it in Step 8.
        bool isLoopBackEdge = false;
        for (const auto &[header, dec] : solution.loopDecisions) {
            bool handledByLoopLogic = dec.mandatoryBackEdge || dec.numIterationsPerCharge > 0;
            if (!handledByLoopLogic || !dec.loop)
                continue;

            llvm::BasicBlock *latch = dec.loop->getLoopLatch();
            if (!latch)
                continue;

            if (edge.src == latch && edge.dst == header) {
                isLoopBackEdge = true;
                break;
            }
        }
        if (isLoopBackEdge)
            continue;

        llvm::BasicBlock *ckptBB = splitEdge(edge.src, edge.dst);
        if (!ckptBB)
            continue;

        const RegionAllocation *endingAlloc = blockToAlloc.lookup(edge.src);
        const RegionAllocation *startingAlloc = blockToAlloc.lookup(edge.dst);

        inserted += insertCheckpointSequence(ckptBB, endingAlloc, startingAlloc, state);
    }

    // Step 8: Handle loop conditional checkpoints.
    for (const auto &[header, decision] : solution.loopDecisions) {
        if (decision.mandatoryBackEdge) {
            // Mandatory: checkpoint every iteration via back-edge.
            llvm::Loop *L = decision.loop;
            llvm::BasicBlock *latch = L ? L->getLoopLatch() : nullptr;
            if (latch) {
                llvm::BasicBlock *ckptBB = splitEdge(latch, header);
                if (ckptBB) {
                    inserted += insertCheckpointSequence(ckptBB, &decision.bodyAllocation,
                                                         &decision.bodyAllocation, state);
                }
            }
        } else if (decision.numIterationsPerCharge > 0) {
            // Conditional: checkpoint every N iterations.
            inserted += insertLoopConditionalCheckpoint(header, decision, state);
        }
    }

    return inserted;
}

} // namespace checkpoint
