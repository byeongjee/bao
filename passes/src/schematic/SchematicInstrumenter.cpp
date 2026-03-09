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

    // Phase 2: Epilogue + register save markers.
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

    // Phase 3: Prologue + register restore markers.
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

    // Phase 4: Restore starting region's VM vars with live_start=true.
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

    // Record PHI values for checkBB before modifying predecessors.
    // SplitEdge replaced latch with checkBB in header's PHIs.
    llvm::SmallVector<std::pair<llvm::PHINode *, llvm::Value *>, 4> phiEntries;
    for (llvm::PHINode &PHI : header->phis()) {
        int idx = PHI.getBasicBlockIndex(checkBB);
        if (idx >= 0)
            phiEntries.push_back({&PHI, PHI.getIncomingValue(idx)});
    }
    // Remove checkBB from PHIs (it will no longer branch directly to header).
    for (auto &[PHI, _] : phiEntries)
        PHI->removeIncomingValue(checkBB, /*DeletePHIIfEmpty=*/false);

    // Reference: compare counter >= (numIt - 2) using signed comparison.
    // Counter starts at 0, incremented only on non-checkpoint path.
    // numIt >= 3 guaranteed (LoopAnalyzer sets mandatory for numIt < 3).
    llvm::IRBuilder<> checkBuilder(checkBB);
    llvm::Value *counterVal = checkBuilder.CreateLoad(I32Ty, counter);
    llvm::Value *threshold = llvm::ConstantInt::get(I32Ty, numIt - 2);
    llvm::Value *cond = checkBuilder.CreateICmpSGE(counterVal, threshold);

    // Create increment BB (counter++, branch to header).
    llvm::BasicBlock *incrBB =
        llvm::BasicBlock::Create(Ctx, "schematic_loop_incr", header->getParent(), header);

    // Create checkpoint BB.
    llvm::BasicBlock *ckptBB =
        llvm::BasicBlock::Create(Ctx, "schematic_loop_ckpt", header->getParent(), header);

    // checkBB: if counter >= threshold, goto ckptBB, else goto incrBB.
    checkBuilder.CreateCondBr(cond, ckptBB, incrBB);

    // incrBB: increment counter, branch to header.
    llvm::IRBuilder<> incrBuilder(incrBB);
    llvm::Value *incremented = incrBuilder.CreateAdd(counterVal, llvm::ConstantInt::get(I32Ty, 1));
    incrBuilder.CreateStore(incremented, counter);
    incrBuilder.CreateBr(header);

    // ckptBB: full save/restore sequence, reset counter to 0, branch to header.
    inserted +=
        insertCheckpointSequence(ckptBB, &decision.bodyAllocation, &decision.bodyAllocation, state);
    llvm::IRBuilder<> ckptBuilder(ckptBB);
    ckptBuilder.CreateStore(llvm::ConstantInt::get(I32Ty, 0), counter);
    ckptBuilder.CreateBr(header);

    // Update PHI nodes: add entries for both incrBB and ckptBB predecessors.
    for (auto &[PHI, val] : phiEntries) {
        PHI->addIncoming(val, incrBB);
        PHI->addIncoming(val, ckptBB);
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

    // Step 4: Rewrite accesses in regions.
    for (const auto &region : solution.regions)
        rewriteAccessesInRegion(region.blocks, region.allocation);

    // Step 5: Insert entry prologue after allocas in entry block.
    {
        llvm::BasicBlock &entryBB = F.getEntryBlock();
        llvm::BasicBlock::iterator insertPt = getInsertPointAfterAllocas(entryBB);
        llvm::IRBuilder<> builder(&entryBB, insertPt);
        builder.CreateCall(prologueFn_);
        inserted++;
    }

    // Step 6: Insert checkpoints at enabled edges.
    // Build a lookup from regions for ending/starting allocations.
    // Map each block to its region allocation.
    llvm::DenseMap<llvm::BasicBlock *, const RegionAllocation *> blockToAlloc;
    for (const auto &region : solution.regions) {
        for (llvm::BasicBlock *BB : region.blocks)
            blockToAlloc[BB] = &region.allocation;
    }

    for (const CFGEdge &edge : solution.enabledCheckpoints) {
        // Skip only the true loop back-edge (latch -> header) when loop
        // checkpoint logic (mandatory or conditional) handles it in Step 7.
        bool isLoopBackEdge = false;
        for (const auto &[header, dec] : solution.loopDecisions) {
            bool handledByLoopLogic =
                dec.mandatoryBackEdge || (dec.numIterationsPerCharge > 0 && !dec.loopFitsEntirely);
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

    // Step 7: Handle loop conditional checkpoints.
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
        } else if (decision.numIterationsPerCharge > 0 && !decision.loopFitsEntirely) {
            // Conditional: checkpoint every N iterations.
            inserted += insertLoopConditionalCheckpoint(header, decision, state);
        }
    }

    return inserted;
}

} // namespace checkpoint
