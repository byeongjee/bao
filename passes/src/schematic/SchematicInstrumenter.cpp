#include "schematic/SchematicInstrumenter.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <set>

namespace checkpoint {

SchematicInstrumenter::SchematicInstrumenter(llvm::Module &M,
                                             bool addDebugMarkers,
                                             unsigned N_reg)
    : M_(M), addDebugMarkers_(addDebugMarkers), N_reg_(N_reg) {
    declareRuntimeFunctions();
}

void SchematicInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
    llvm::Type *PtrTy = llvm::PointerType::getUnqual(Ctx);

    llvm::Type *I64Ty = llvm::Type::getInt64Ty(Ctx);

    prologueFn_ = M_.getOrInsertFunction("__region_prologue", VoidTy);
    epilogueFn_ = M_.getOrInsertFunction("__region_epilogue", VoidTy);

    // Always declare register save/restore (needed for N_reg blanket model)
    storeRegFn_ = M_.getOrInsertFunction(
        "__checkpoint_store_reg", VoidTy, I32Ty, I64Ty);
    restoreRegFn_ = M_.getOrInsertFunction(
        "__restore_reg", VoidTy, I32Ty, PtrTy);

    if (addDebugMarkers_) {
        storeMemFn_ = M_.getOrInsertFunction(
            "__checkpoint_store_mem", VoidTy, PtrTy, PtrTy, I32Ty);
        restoreMemFn_ = M_.getOrInsertFunction(
            "__restore_mem", VoidTy, PtrTy, PtrTy, I32Ty);
    }
}

void SchematicInstrumenter::createShadowGlobals(
    llvm::Function &F,
    const SchematicSolution &solution,
    const StateAnalysis &state) {

    shadowMap_.clear();

    // Collect candidate GVs that are VM-placed in any region
    std::set<llvm::GlobalVariable *> vmPlacedGVs;
    for (const auto &region : solution.regions) {
        for (const auto &[gv, p] : region.allocation.placement) {
            if (p == Placement::VM)
                vmPlacedGVs.insert(gv);
        }
    }

    for (llvm::GlobalVariable *GV : vmPlacedGVs) {
        auto *shadow = new llvm::GlobalVariable(
            M_, GV->getValueType(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(GV->getValueType()),
            "__vm_shadow_" + GV->getName().str());
        shadow->setAlignment(GV->getAlign());
        shadowMap_[GV] = shadow;
    }
}

llvm::BasicBlock *SchematicInstrumenter::splitEdge(llvm::BasicBlock *src,
                                                    llvm::BasicBlock *dst) {
    return llvm::SplitEdge(src, dst);
}

/// Recursively replace occurrences of GV with Replacement inside a Constant.
static llvm::Constant *replaceGVInConstant(llvm::Constant *C,
                                            llvm::GlobalVariable *GV,
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

void SchematicInstrumenter::rewriteAccessesInRegion(
    const std::vector<llvm::BasicBlock *> &blocks,
    const RegionAllocation &allocation) {

    for (llvm::BasicBlock *BB : blocks) {
        for (const auto &[GV, shadow] : shadowMap_) {
            // Check if this GV is VM-placed in this region
            auto it = allocation.placement.find(GV);
            if (it == allocation.placement.end() || it->second != Placement::VM)
                continue;

            // Replace uses of GV in this block with shadow
            for (llvm::Instruction &I : *BB) {
                for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                    if (I.getOperand(i) == GV) {
                        I.setOperand(i, shadow);
                    } else if (auto *C = llvm::dyn_cast<llvm::Constant>(
                                   I.getOperand(i))) {
                        if (auto *replaced = replaceGVInConstant(C, GV, shadow))
                            I.setOperand(i, replaced);
                    }
                }
            }
        }
    }
}

unsigned SchematicInstrumenter::insertCheckpointSequence(
    llvm::BasicBlock *ckptBB,
    const RegionAllocation *endingAlloc,
    const RegionAllocation *startingAlloc,
    const StateAnalysis &state) {

    unsigned inserted = 0;
    llvm::IRBuilder<> builder(ckptBB, ckptBB->getFirstInsertionPt());

    // If there's a terminator already (from SplitEdge), insert before it
    if (ckptBB->getTerminator()) {
        builder.SetInsertPoint(ckptBB->getTerminator());
    }

    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
    llvm::Type *I64Ty = llvm::Type::getInt64Ty(Ctx);
    llvm::Type *PtrTy = llvm::PointerType::getUnqual(Ctx);

    // 1. Save registers (N_reg calls with dummy values)
    for (unsigned r = 0; r < N_reg_; ++r) {
        builder.CreateCall(storeRegFn_,
                           {llvm::ConstantInt::get(I32Ty, r),
                            llvm::ConstantInt::get(I64Ty, 0)});
        inserted++;
    }

    // 2. Save VM variables from ending region (filtered by live_end)
    if (endingAlloc) {
        for (const auto &[gv, p] : endingAlloc->placement) {
            if (p != Placement::VM) continue;
            // Filter by liveness: only save if live_end is true
            auto liveIt = endingAlloc->livenessFlags.find(gv);
            if (liveIt != endingAlloc->livenessFlags.end() &&
                !liveIt->second.second)
                continue; // live_end is false, skip save

            auto shadowIt = shadowMap_.find(gv);
            if (shadowIt == shadowMap_.end()) continue;

            unsigned sizeBytes = state.getVarSizeBytes(gv);
            if (sizeBytes == 0) continue;

            llvm::Value *size = llvm::ConstantInt::get(I32Ty, sizeBytes);

            if (addDebugMarkers_) {
                builder.CreateCall(storeMemFn_,
                                   {gv, shadowIt->second, size});
            } else {
                builder.CreateMemCpy(
                    gv, gv->getAlign(),
                    shadowIt->second, shadowIt->second->getAlign(),
                    size);
            }
            inserted++;
        }
    }

    // 3. Epilogue
    builder.CreateCall(epilogueFn_);
    inserted++;

    // 4. Prologue
    builder.CreateCall(prologueFn_);
    inserted++;

    // 5. Restore registers (N_reg calls with null pointers)
    for (unsigned r = 0; r < N_reg_; ++r) {
        builder.CreateCall(restoreRegFn_,
                           {llvm::ConstantInt::get(I32Ty, r),
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(PtrTy))});
        inserted++;
    }

    // 6. Restore VM variables for starting region (filtered by live_start)
    if (startingAlloc) {
        for (const auto &[gv, p] : startingAlloc->placement) {
            if (p != Placement::VM) continue;
            // Filter by liveness: only restore if live_start is true
            auto liveIt = startingAlloc->livenessFlags.find(gv);
            if (liveIt != startingAlloc->livenessFlags.end() &&
                !liveIt->second.first)
                continue; // live_start is false, skip restore

            auto shadowIt = shadowMap_.find(gv);
            if (shadowIt == shadowMap_.end()) continue;

            unsigned sizeBytes = state.getVarSizeBytes(gv);
            if (sizeBytes == 0) continue;

            llvm::Value *size = llvm::ConstantInt::get(I32Ty, sizeBytes);

            if (addDebugMarkers_) {
                builder.CreateCall(restoreMemFn_,
                                   {shadowIt->second, gv, size});
            } else {
                builder.CreateMemCpy(
                    shadowIt->second, shadowIt->second->getAlign(),
                    gv, gv->getAlign(),
                    size);
            }
            inserted++;
        }
    }

    return inserted;
}

unsigned SchematicInstrumenter::insertLoopConditionalCheckpoint(
    llvm::BasicBlock *header,
    const LoopCheckpointDecision &decision,
    const StateAnalysis &state) {

    if (decision.numIterationsPerCharge == 0 || !decision.loop)
        return 0;

    llvm::Loop *L = decision.loop;
    llvm::BasicBlock *preheader = L->getLoopPreheader();
    llvm::BasicBlock *latch = L->getLoopLatch();
    if (!preheader || !latch) return 0;

    unsigned inserted = 0;
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);

    // Counter alloca in preheader — will be promoted to SSA by mem2reg
    // (added to the pass pipeline after SchematicPass).
    // Initialize to 0: the first back-edge crossing intentionally triggers a
    // checkpoint (0 % N == 0) because the initial charge cycle starts at
    // function entry with a prologue, so iteration 0 establishes the first
    // region boundary.
    llvm::IRBuilder<> preBuilder(preheader->getTerminator());
    llvm::AllocaInst *counter = preBuilder.CreateAlloca(I32Ty, nullptr,
                                                         "schematic_loop_ctr");
    preBuilder.CreateStore(llvm::ConstantInt::get(I32Ty, 0), counter);
    inserted++;

    // In latch: load counter, check counter % numIt == 0, branch to checkpoint
    // Split the latch to insert the check before the back-edge
    llvm::BasicBlock *checkBB = llvm::SplitEdge(latch, header);
    if (!checkBB) return inserted;

    // Create checkpoint block
    llvm::BasicBlock *ckptBB = llvm::BasicBlock::Create(
        Ctx, "schematic_loop_ckpt", header->getParent(), header);

    llvm::IRBuilder<> checkBuilder(checkBB->getTerminator());

    // Load and check counter
    llvm::Value *ctrVal = checkBuilder.CreateLoad(I32Ty, counter);
    llvm::Value *numItVal = llvm::ConstantInt::get(
        I32Ty, decision.numIterationsPerCharge);
    llvm::Value *rem = checkBuilder.CreateURem(ctrVal, numItVal);
    llvm::Value *needsCkpt = checkBuilder.CreateICmpEQ(
        rem, llvm::ConstantInt::get(I32Ty, 0));

    // Increment counter
    llvm::Value *newCtr = checkBuilder.CreateAdd(
        ctrVal, llvm::ConstantInt::get(I32Ty, 1));
    checkBuilder.CreateStore(newCtr, counter);

    // Replace unconditional branch with conditional
    checkBuilder.CreateCondBr(needsCkpt, ckptBB, header);
    checkBB->getTerminator()->eraseFromParent(); // remove old branch

    // Insert checkpoint sequence in ckptBB:
    // save regs → save VM vars → epilogue → prologue → restore regs → restore VM vars
    llvm::IRBuilder<> ckptBuilder(ckptBB);
    llvm::Type *I64Ty = llvm::Type::getInt64Ty(Ctx);
    llvm::Type *PtrTy = llvm::PointerType::getUnqual(Ctx);

    // Save registers
    for (unsigned r = 0; r < N_reg_; ++r) {
        ckptBuilder.CreateCall(storeRegFn_,
                               {llvm::ConstantInt::get(I32Ty, r),
                                llvm::ConstantInt::get(I64Ty, 0)});
        inserted++;
    }

    // Save VM variables from loop body to NVM backing store
    // (no liveness filtering for loop — consistent with loop energy model)
    for (const auto &[gv, p] : decision.bodyAllocation.placement) {
        if (p != Placement::VM) continue;
        auto shadowIt = shadowMap_.find(gv);
        if (shadowIt == shadowMap_.end()) continue;
        unsigned sizeBytes = state.getVarSizeBytes(gv);
        if (sizeBytes == 0) continue;
        llvm::Value *size = llvm::ConstantInt::get(I32Ty, sizeBytes);
        if (addDebugMarkers_) {
            ckptBuilder.CreateCall(storeMemFn_, {gv, shadowIt->second, size});
        } else {
            ckptBuilder.CreateMemCpy(gv, gv->getAlign(),
                                     shadowIt->second, shadowIt->second->getAlign(),
                                     size);
        }
        inserted++;
    }

    ckptBuilder.CreateCall(epilogueFn_);
    ckptBuilder.CreateCall(prologueFn_);
    inserted += 2;

    // Restore registers
    for (unsigned r = 0; r < N_reg_; ++r) {
        ckptBuilder.CreateCall(restoreRegFn_,
                               {llvm::ConstantInt::get(I32Ty, r),
                                llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(PtrTy))});
        inserted++;
    }

    // Restore VM variables from NVM backing store
    // (no liveness filtering for loop — consistent with loop energy model)
    for (const auto &[gv, p] : decision.bodyAllocation.placement) {
        if (p != Placement::VM) continue;
        auto shadowIt = shadowMap_.find(gv);
        if (shadowIt == shadowMap_.end()) continue;
        unsigned sizeBytes = state.getVarSizeBytes(gv);
        if (sizeBytes == 0) continue;
        llvm::Value *size = llvm::ConstantInt::get(I32Ty, sizeBytes);
        if (addDebugMarkers_) {
            ckptBuilder.CreateCall(restoreMemFn_, {shadowIt->second, gv, size});
        } else {
            ckptBuilder.CreateMemCpy(shadowIt->second, shadowIt->second->getAlign(),
                                     gv, gv->getAlign(),
                                     size);
        }
        inserted++;
    }

    ckptBuilder.CreateBr(header);

    // Update header's PHI nodes: ckptBB is a new predecessor.
    // ckptBB is a pass-through for all loop variables (it only calls
    // epilogue/prologue), so the incoming values are the same as from checkBB.
    for (llvm::PHINode &PHI : header->phis()) {
        llvm::Value *incomingVal = PHI.getIncomingValueForBlock(checkBB);
        PHI.addIncoming(incomingVal, ckptBB);
    }

    return inserted;
}

unsigned SchematicInstrumenter::instrumentFunction(
    llvm::Function &F,
    const SchematicSolution &solution,
    const StateAnalysis &state) {

    createShadowGlobals(F, solution, state);

    unsigned totalInserted = 0;

    // Insert prologue at function entry
    llvm::BasicBlock &entryBB = F.getEntryBlock();
    llvm::IRBuilder<> entryBuilder(&*entryBB.getFirstInsertionPt());
    entryBuilder.CreateCall(prologueFn_);
    totalInserted++;

    // Insert epilogue before each return
    for (llvm::BasicBlock &BB : F) {
        auto *term = BB.getTerminator();
        if (llvm::isa<llvm::ReturnInst>(term)) {
            llvm::IRBuilder<> retBuilder(term);
            retBuilder.CreateCall(epilogueFn_);
            totalInserted++;
        }
    }

    // Build a map from edge to region indices for finding ending/starting allocations
    // For each enabled checkpoint edge, find the regions before and after
    std::map<CFGEdge, size_t> edgeToRegionAfter;
    std::map<CFGEdge, size_t> edgeToRegionBefore;

    // Build region lookup: block -> region index
    std::map<llvm::BasicBlock *, size_t> blockToRegion;
    for (size_t i = 0; i < solution.regions.size(); ++i) {
        for (llvm::BasicBlock *BB : solution.regions[i].blocks) {
            blockToRegion[BB] = i;
        }
    }

    // For each checkpoint edge, find ending and starting regions
    for (const auto &edge : solution.enabledCheckpoints) {
        auto srcIt = blockToRegion.find(edge.src);
        auto dstIt = blockToRegion.find(edge.dst);
        if (srcIt != blockToRegion.end())
            edgeToRegionBefore[edge] = srcIt->second;
        if (dstIt != blockToRegion.end())
            edgeToRegionAfter[edge] = dstIt->second;
    }

    // Insert checkpoint sequences at enabled edges
    for (const auto &edge : solution.enabledCheckpoints) {
        // Check if this is a loop back-edge handled by conditional checkpoint
        bool isLoopConditional = false;
        auto loopIt = solution.loopDecisions.find(edge.dst);
        if (loopIt != solution.loopDecisions.end()) {
            const auto &dec = loopIt->second;
            if (dec.numIterationsPerCharge > 1) {
                // Handled by conditional checkpoint
                isLoopConditional = true;
            }
        }

        if (isLoopConditional) continue; // Will be handled below

        llvm::BasicBlock *ckptBB = splitEdge(edge.src, edge.dst);
        if (!ckptBB) continue;

        const RegionAllocation *endingAlloc = nullptr;
        const RegionAllocation *startingAlloc = nullptr;

        auto beforeIt = edgeToRegionBefore.find(edge);
        if (beforeIt != edgeToRegionBefore.end())
            endingAlloc = &solution.regions[beforeIt->second].allocation;

        auto afterIt = edgeToRegionAfter.find(edge);
        if (afterIt != edgeToRegionAfter.end())
            startingAlloc = &solution.regions[afterIt->second].allocation;

        totalInserted += insertCheckpointSequence(
            ckptBB, endingAlloc, startingAlloc, state);
    }

    // Insert loop conditional checkpoints
    for (const auto &[header, decision] : solution.loopDecisions) {
        if (decision.numIterationsPerCharge > 1) {
            totalInserted += insertLoopConditionalCheckpoint(
                header, decision, state);
        }
    }

    // Rewrite memory accesses in each region
    for (const auto &region : solution.regions) {
        rewriteAccessesInRegion(region.blocks, region.allocation);
    }

    return totalInserted;
}

} // namespace checkpoint
