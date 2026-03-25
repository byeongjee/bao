#include "schematic/SchematicInstrumenter.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <cassert>
#include <queue>
#include <set>

namespace checkpoint {

SchematicInstrumenter::SchematicInstrumenter(llvm::Module &M, bool addDebugMarkers)
    : M_(M), addDebugMarkers_(addDebugMarkers) {}

void SchematicInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);

    boundaryFn_ = M_.getOrInsertFunction("__region_boundary", VoidTy);

    if (addDebugMarkers_) {
        llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
        cntStoreMemGV_ =
            llvm::dyn_cast<llvm::GlobalVariable>(M_.getOrInsertGlobal("cnt_store_mem", I32Ty));
        assert(cntStoreMemGV_ && "cnt_store_mem must be a GlobalVariable");
        cntStoreMemGV_->setLinkage(llvm::GlobalValue::ExternalLinkage);

        cntRestoreMemGV_ =
            llvm::dyn_cast<llvm::GlobalVariable>(M_.getOrInsertGlobal("cnt_restore_mem", I32Ty));
        assert(cntRestoreMemGV_ && "cnt_restore_mem must be a GlobalVariable");
        cntRestoreMemGV_->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
}

void SchematicInstrumenter::createShadowGlobals(llvm::Function &F,
                                                const SchematicSolution &solution,
                                                const SchematicStateAnalysis &state) {

    shadowMap_.clear();

    // Collect all candidates (globals + allocas) that have Placement::VM in any region.
    std::set<llvm::Value *> vmPlacedVals;
    for (const auto &region : solution.regions) {
        for (const auto &[v, va] : region.allocation.vars) {
            if (va.placement == Placement::VM)
                vmPlacedVals.insert(v);
        }
    }
    // Also check loop decisions.
    for (const auto &[header, dec] : solution.loopDecisions) {
        for (const auto &[v, va] : dec.bodyAllocation.vars) {
            if (va.placement == Placement::VM)
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
        for (const auto &[V, va] : allocation.vars) {
            if (va.placement != Placement::VM)
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

void SchematicInstrumenter::rewriteAllShadowAccesses(llvm::Function &F) {
    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                llvm::Value *Op = I.getOperand(i);

                auto it = shadowMap_.find(Op);
                if (it != shadowMap_.end()) {
                    I.setOperand(i, it->second);
                    continue;
                }

                // Handle constant expressions that reference shadow-mapped globals.
                if (auto *C = llvm::dyn_cast<llvm::Constant>(Op)) {
                    for (const auto &[V, shadow] : shadowMap_) {
                        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
                            if (auto *replaced = replaceGVInConstant(C, GV, shadow)) {
                                I.setOperand(i, replaced);
                                C = replaced; // chain replacements
                            }
                        }
                    }
                }
            }
        }
    }
}

/// Emit an inline increment of an NVM counter global: load, add 1, store.
static void emitCounterIncrement(llvm::IRBuilder<> &builder, llvm::GlobalVariable *counterGV) {
    llvm::Type *counterTy = counterGV->getValueType();
    llvm::Value *val = builder.CreateLoad(counterTy, counterGV);
    llvm::Value *inc = builder.CreateAdd(val, llvm::ConstantInt::get(counterTy, 1));
    builder.CreateStore(inc, counterGV);
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
        for (const auto &[v, va] : endingAlloc->vars) {
            if (!va.needSave())
                continue;

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
                    emitCounterIncrement(builder, cntStoreMemGV_);
            } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                builder.CreateMemCpy(AI, AI->getAlign(), shadowIt->second,
                                     shadowIt->second->getAlign(), size);
                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntStoreMemGV_);
            }
            inserted++;
            storeMemCalls_++;
        }
    }

    // Phase 2: Call __region_boundary.
    // Assembly handles bulk register save/restore, cnt_boundary, cnt_save_reg,
    // cnt_restore_reg internally via #ifdef DEVICE_DEBUG.
    builder.CreateCall(boundaryFn_);
    inserted++;
    boundaryCalls_++;

    // Phase 3: Restore starting region's VM vars with live_start=true.
    // For globals: memcpy GV -> shadow (FRAM -> SRAM).
    // For allocas: memcpy alloca -> shadow (FRAM stack -> SRAM).
    if (startingAlloc) {
        for (const auto &[v, va] : startingAlloc->vars) {
            if (!va.needRestore())
                continue;

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
                    emitCounterIncrement(builder, cntRestoreMemGV_);
            } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(v)) {
                builder.CreateMemCpy(shadowIt->second, shadowIt->second->getAlign(), AI,
                                     AI->getAlign(), size);
                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntRestoreMemGV_);
            }
            inserted++;
            restoreMemCalls_++;
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

    // Split the back-edge (latch -> header) using SplitEdge.
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

unsigned SchematicInstrumenter::instrumentFunction(llvm::Function &F, SchematicSolution &solution,
                                                   const SchematicStateAnalysis &state) {

    unsigned inserted = 0;
    boundaryCalls_ = 0;
    storeMemCalls_ = 0;
    restoreMemCalls_ = 0;

    // Step 1: Declare runtime functions.
    declareRuntimeFunctions();

    // Step 2: Ensure candidate globals have a FRAM section if not already annotated.
    for (llvm::Value *V : state.getCandidates()) {
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
            if (GV->getSection().empty())
                GV->setSection(".fram");
        }
    }

    // Step 3: Create shadow globals.
    createShadowGlobals(F, solution, state);

    // Step 4: Build per-block allocation map from blockAllocation.
    // Allocations were already extended during analysis (SchematicPass Step 9c)
    // to contain all accessed candidate variables.
    llvm::DenseMap<llvm::BasicBlock *, const RegionAllocation *> blockToAlloc;
    for (const auto &[sblock, allocPtr] : solution.blockAllocation) {
        if (sblock->isSynthetic() || !allocPtr)
            continue;
        if (auto *BB = sblock->getLLVMBlock())
            blockToAlloc[BB] = allocPtr.get();
    }

    // Step 5: Rewrite accesses per-block based on each block's allocation.
    // VM-placed variables get rewritten to use the shadow global.
    // NVM-placed variables keep the original alloca/GV.
    for (llvm::BasicBlock &BB : F) {
        auto it = blockToAlloc.find(&BB);
        if (it == blockToAlloc.end())
            continue;
        rewriteAccessesInRegion({&BB}, *it->second);
    }

    // Assertion: no two SchematicBlock* entries in blockAllocation should map to
    // the same BasicBlock*. If they do, the last one wins in blockToAlloc and the
    // rewriting/checkpoint logic may use inconsistent allocations.
    {
        llvm::DenseMap<llvm::BasicBlock *, const SchematicBlock *> seen;
        for (const auto &[sblock, allocPtr] : solution.blockAllocation) {
            if (sblock->isSynthetic() || !allocPtr)
                continue;
            auto *BB = sblock->getLLVMBlock();
            if (!BB)
                continue;
            auto it = seen.find(BB);
            if (it != seen.end()) {
                llvm::errs() << "SCHEMATIC: duplicate blockAllocation for BB '" << BB->getName()
                             << "': SchematicBlock '" << it->second->getName() << "' and '"
                             << sblock->getName() << "'\n";
                assert(false && "Duplicate BasicBlock in blockAllocation");
            }
            seen[BB] = sblock;
        }
    }

    // Step 7: Entry block — no boundary call (consistent with RockClimb,
    // which skips the entry block boundary). The first region starts at
    // program entry without a checkpoint.

    // Step 8: Insert checkpoints at enabled edges.

    for (const CFGEdge &edge : solution.enabledCheckpoints) {
        // Skip synthetic blocks — they have no real IR.
        if (edge.src->isSynthetic() || edge.dst->isSynthetic())
            continue;

        llvm::BasicBlock *srcBB = edge.src->getLLVMBlock();
        llvm::BasicBlock *dstBB = edge.dst->getLLVMBlock();

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

            llvm::BasicBlock *headerBB = header->getLLVMBlock();
            if (srcBB == latch && dstBB == headerBB) {
                isLoopBackEdge = true;
                break;
            }
        }
        if (isLoopBackEdge)
            continue;

        const RegionAllocation *endingAlloc = blockToAlloc.lookup(srcBB);
        const RegionAllocation *startingAlloc = blockToAlloc.lookup(dstBB);

        // Check if srcBB → dstBB is a direct LLVM CFG edge.
        bool isDirect = false;
        for (llvm::BasicBlock *succ : llvm::successors(srcBB)) {
            if (succ == dstBB) {
                isDirect = true;
                break;
            }
        }

        if (isDirect) {
            llvm::BasicBlock *ckptBB = splitEdge(srcBB, dstBB);
            if (ckptBB)
                inserted += insertCheckpointSequence(ckptBB, endingAlloc, startingAlloc, state);
        } else {
            // Non-adjacent edge (from loop-collapsed trace).  The checkpoint
            // logically belongs at the entry to dstBB.  Split every predecessor
            // edge of dstBB that comes from a block reachable from srcBB
            // (i.e., intermediate blocks in the collapsed region).
            llvm::SmallSetVector<llvm::BasicBlock *, 4> reachable;
            std::queue<llvm::BasicBlock *> bfsQueue;
            bfsQueue.push(srcBB);
            reachable.insert(srcBB);
            while (!bfsQueue.empty()) {
                llvm::BasicBlock *cur = bfsQueue.front();
                bfsQueue.pop();
                for (llvm::BasicBlock *succ : llvm::successors(cur)) {
                    if (succ == dstBB || reachable.count(succ))
                        continue;
                    reachable.insert(succ);
                    bfsQueue.push(succ);
                }
            }

            // Collect predecessors first (splitting modifies the predecessor list).
            llvm::SmallVector<llvm::BasicBlock *, 4> predsToSplit;
            for (llvm::BasicBlock *pred : llvm::predecessors(dstBB)) {
                if (reachable.count(pred))
                    predsToSplit.push_back(pred);
            }
            for (llvm::BasicBlock *pred : predsToSplit) {
                llvm::BasicBlock *ckptBB = splitEdge(pred, dstBB);
                if (ckptBB) {
                    const RegionAllocation *predAlloc = blockToAlloc.lookup(pred);
                    inserted += insertCheckpointSequence(ckptBB, predAlloc, startingAlloc, state);
                }
            }
        }
    }

    // Step 7: Handle loop conditional checkpoints.
    for (const auto &[header, decision] : solution.loopDecisions) {
        // Skip synthetic headers (shouldn't happen, but guard).
        if (header->isSynthetic())
            continue;
        llvm::BasicBlock *headerBB = header->getLLVMBlock();

        if (decision.mandatoryBackEdge) {
            // Mandatory: checkpoint every iteration via back-edge.
            llvm::Loop *L = decision.loop;
            llvm::BasicBlock *latch = L ? L->getLoopLatch() : nullptr;
            if (latch) {
                llvm::BasicBlock *ckptBB = splitEdge(latch, headerBB);
                if (ckptBB) {
                    inserted += insertCheckpointSequence(ckptBB, &decision.bodyAllocation,
                                                         &decision.bodyAllocation, state);
                }
            }
        } else if (decision.numIterationsPerCharge > 0 && !decision.loopFitsEntirely) {
            // Conditional: checkpoint every N iterations.
            inserted += insertLoopConditionalCheckpoint(headerBB, decision, state);
        }
    }

    return inserted;
}

} // namespace checkpoint
