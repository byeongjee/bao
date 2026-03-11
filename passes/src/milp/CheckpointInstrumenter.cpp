#include "milp/CheckpointInstrumenter.h"
#include "common/BlockUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"

#include <cassert>
#include <set>
#include <vector>

namespace checkpoint {

CheckpointInstrumenter::CheckpointInstrumenter(llvm::Module &M, bool addDebugMarkers)
    : M_(M), addDebugMarkers_(addDebugMarkers) {
    declareRuntimeFunctions();
}

void CheckpointInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);

    boundaryFn_ = M_.getOrInsertFunction("__region_boundary", VoidTy);

    if (addDebugMarkers_) {
        llvm::Type *I16Ty = llvm::Type::getInt16Ty(Ctx);
        auto getOrCreateCounter = [&](const char *name) -> llvm::GlobalVariable * {
            if (auto *existing = M_.getGlobalVariable(name))
                return existing;
            auto *GV = new llvm::GlobalVariable(M_, I16Ty, /*isConstant=*/false,
                                                llvm::GlobalValue::ExternalLinkage,
                                                /*Initializer=*/nullptr, name);
            return GV;
        };
        // cnt_save_vreg/cnt_restore_vreg count IR-level value saves which may
        // not map 1:1 to physical register saves due to register spilling.
        // TODO: Post-regalloc pass for exact physical register counting.
        cntSaveVregGV_ = getOrCreateCounter("cnt_save_vreg");
        cntRestoreVregGV_ = getOrCreateCounter("cnt_restore_vreg");
        cntStoreMemGV_ = getOrCreateCounter("cnt_store_mem");
        cntRestoreMemGV_ = getOrCreateCounter("cnt_restore_mem");
    }
}

void CheckpointInstrumenter::emitCounterIncrement(llvm::IRBuilder<> &builder,
                                                  llvm::GlobalVariable *counter) {
    llvm::Value *val = builder.CreateLoad(counter->getValueType(), counter);
    llvm::Value *inc = builder.CreateAdd(val, llvm::ConstantInt::get(counter->getValueType(), 1));
    builder.CreateStore(inc, counter);
}

unsigned CheckpointInstrumenter::instrumentFunction(llvm::Function &F, const MILPSolution &solution,
                                                    const ICFGView &cfg,
                                                    const IStateView &stateView,
                                                    const StateAnalysis &state) {

    applyMemoryPlacement(state);
    createShadowGlobals(F, solution, state);
    createNVMBackupGlobals(F, solution, state, cfg);
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
    std::set<llvm::GlobalVariable *> vmPlacedGVs;
    for (const auto &[key, placed] : solution.placeInVm) {
        if (placed && !state.isIneligible(key.second)) {
            vmPlacedGVs.insert(key.second);
        }
    }

    for (llvm::GlobalVariable *GV : vmPlacedGVs) {
        auto *shadow = new llvm::GlobalVariable(
            M_, GV->getValueType(), /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(GV->getValueType()), "__vm_shadow_" + GV->getName().str());
        shadow->setAlignment(GV->getAlign());
        shadowMap_[GV] = shadow;
    }
}

void CheckpointInstrumenter::createNVMBackupGlobals(llvm::Function &F, const MILPSolution &solution,
                                                    const StateAnalysis &state,
                                                    const ICFGView &cfg) {

    nvmBackupMap_.clear();

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
            continue;
        }

        auto *backup = new llvm::GlobalVariable(
            M_, backupType, /*isConstant=*/false, llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(backupType), backupName);
        backup->setSection(".nvm");
        nvmBackupMap_[V] = backup;
    }
}

/// Recursively replace occurrences of \p GV with \p Replacement inside a
/// Constant (e.g. a ConstantExpr GEP that embeds a global pointer).
/// Returns the rebuilt Constant on success, or nullptr if \p C does not
/// reference \p GV.
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

void CheckpointInstrumenter::rewriteAccessesInVMRegions(llvm::Function &F,
                                                        const MILPSolution &solution,
                                                        const ICFGView &cfg) {

    const NodeMap &nodeMap = cfg.getNodeMap();
    for (llvm::BasicBlock &BB : F) {
        NodeId nodeId = nodeMap.getNodeId(&BB);
        if (nodeId == kInvalidNodeId)
            continue;

        for (auto &[GV, shadow] : shadowMap_) {
            auto key = std::make_pair(nodeId, GV);
            auto pvIt = solution.placeInVm.find(key);
            if (pvIt == solution.placeInVm.end() || !pvIt->second)
                continue;

            // Replace uses of GV in this block with shadow.
            // Handles both direct operands and GV references nested
            // inside ConstantExpr operands (e.g. constant-index GEPs
            // created by loop unrolling at -O2).
            for (llvm::Instruction &I : BB) {
                for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                    if (I.getOperand(i) == GV) {
                        I.setOperand(i, shadow);
                    } else if (auto *C = llvm::dyn_cast<llvm::Constant>(I.getOperand(i))) {
                        if (auto *replaced = replaceGVInConstant(C, GV, shadow))
                            I.setOperand(i, replaced);
                    }
                }
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
    for (NodeId node : solution.regionStarts) {
        llvm::BasicBlock *BB = cfg.getNodeMap().getConcreteBlock(node);
        if (!BB || BB->getParent() != &F) {
            llvm::errs() << "CheckpointInstrumenter: unresolved region-start node "
                         << cfg.getNodeName(node) << "\n";
            continue;
        }
        nodesByBlock[BB].push_back(node);
    }

    // Accumulated across all boundary blocks for Phase 2 SSA rewriting.
    std::map<llvm::Value *, std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>>>
        ssaRestoreDefs;
    std::set<llvm::Instruction *> allCommitInsts;
    // Deferred SSA commit stores: origVal → [(nvmBackup, insertBefore), ...]
    std::map<llvm::Value *, std::vector<std::pair<llvm::GlobalVariable *, llvm::Instruction *>>>
        pendingSSACommits;

    // Phase 1: Insert commit stores, region boundaries, and restore loads
    // at each boundary block. Accumulate SSA restore definitions for Phase 2.
    for (llvm::BasicBlock &BB : F) {
        auto itNodeVec = nodesByBlock.find(&BB);
        if (itNodeVec == nodesByBlock.end()) {
            continue;
        }

        const std::vector<NodeId> &nodes = itNodeVec->second;
        std::set<NodeId> nodeSet(nodes.begin(), nodes.end());

        llvm::BasicBlock::iterator insertIt = checkpoint::getInsertPointAfterAllocas(BB);
        if (insertIt == BB.end()) {
            continue;
        }

        llvm::IRBuilder<> builder(&BB, insertIt);

        bool isEntryNode = nodeSet.count(entryNode) > 0;

        // Track commit instructions so SSA use rewriting skips them.
        std::set<llvm::Instruction *> commitInsts;

        // For b != b0, emit boundary-end code first.
        // Order: commit stores -> region boundary -> restores
        if (!isEntryNode) {
            // Deferred SSA commits for this boundary block.
            std::vector<std::pair<llvm::Value *, llvm::GlobalVariable *>> ssaCommitsHere;

            // Commit dirty data while the region is still active.
            std::set<llvm::Value *> commitVars;
            for (NodeId n : nodeSet)
                for (llvm::Value *V : solution.getCommitVarsAt(n))
                    commitVars.insert(V);

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
                    // Eligible: copy shadow (VM/SRAM) -> original (NVM/FRAM)
                    auto *mc = builder.CreateMemCpy(GV, GV->getAlign(), shadowIt->second,
                                                    shadowIt->second->getAlign(), size);
                    commitInsts.insert(mc);
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntStoreMemGV_);
                } else if (llvm::isa<llvm::AllocaInst>(V)) {
                    // Stack alloca -> NVM backup (memcpy)
                    llvm::Value *size =
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() &&
                           "alloca commit requires an NVM backup");
                    {
                        auto *AI = llvm::cast<llvm::AllocaInst>(V);
                        auto *mc =
                            builder.CreateMemCpy(backupIt->second, backupIt->second->getAlign(), AI,
                                                 AI->getAlign(), size);
                        commitInsts.insert(mc);
                    }
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntStoreMemGV_);
                } else {
                    // SSA register: defer — direct store may violate dominance.
                    // Phase 2 will use SSAUpdater to find the reaching def.
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() && "SSA commit requires an NVM backup");
                    ssaCommitsHere.emplace_back(V, backupIt->second);
                }
                inserted++;
            }

            // Insert region boundary (replaces old epilogue + prologue pair).
            auto *boundaryCall = builder.CreateCall(boundaryFn_);
            inserted++;

            // Transfer deferred SSA commits with the boundary call as insertion point.
            for (auto &[origVal, nvmBackup] : ssaCommitsHere) {
                pendingSSACommits[origVal].emplace_back(nvmBackup, boundaryCall);
            }
        }
        // Entry node: no boundary call at program start.

        // Eligible restores (needRestore).
        std::set<llvm::GlobalVariable *> restoreGVs;
        for (NodeId n : nodeSet)
            for (llvm::GlobalVariable *GV : solution.getRestoreGVsAt(n))
                restoreGVs.insert(GV);

        for (llvm::GlobalVariable *GV : restoreGVs) {
            unsigned sizeBytes = state.getVarSizeBytes(GV);
            if (sizeBytes == 0)
                continue;
            llvm::Value *size =
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
            // Restore: copy original (NVM/FRAM) -> shadow (VM/SRAM)
            auto shadowIt = shadowMap_.find(GV);
            assert(shadowIt != shadowMap_.end() &&
                   "needRestore requires a VM shadow for the global");
            builder.CreateMemCpy(shadowIt->second, shadowIt->second->getAlign(), GV, GV->getAlign(),
                                 size);
            if (addDebugMarkers_)
                emitCounterIncrement(builder, cntRestoreMemGV_);
            inserted++;
        }

        // Ineligible restores: unconditional at every region start where
        // the object is live-in.
        for (llvm::Value *V : state.getIneligibleObjs()) {
            auto backupIt = nvmBackupMap_.find(V);
            if (backupIt == nvmBackupMap_.end())
                continue;

            // Check if V is live-in at any node mapped to this block.
            bool isLiveIn = false;
            for (NodeId node : nodes) {
                if (stateView.getIneligLiveIn(node).count(V)) {
                    isLiveIn = true;
                    break;
                }
            }
            if (!isLiveIn)
                continue;

            if (llvm::isa<llvm::AllocaInst>(V)) {
                // Stack alloca: memcpy from NVM backup.
                unsigned sizeBytes = state.getVarSizeBytes(V);
                if (sizeBytes == 0)
                    continue;
                llvm::Value *size =
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                // Restore: copy NVM backup -> SRAM original
                auto *AI = llvm::cast<llvm::AllocaInst>(V);
                builder.CreateMemCpy(AI, AI->getAlign(), backupIt->second,
                                     backupIt->second->getAlign(), size);
                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntRestoreMemGV_);
                inserted++;
            } else {
                // SSA register: typed load from NVM slot.
                llvm::Value *restored = builder.CreateLoad(V->getType(), backupIt->second);
                ssaRestoreDefs[V].emplace_back(&BB, restored);
                inserted++;

                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntRestoreVregGV_);
            }
        }

        allCommitInsts.insert(commitInsts.begin(), commitInsts.end());
    }

    // Phase 2: Resolve deferred SSA commits via SSAUpdater.
    // For each SSA value with pending commits, use SSAUpdater to find the
    // reaching definition at each boundary block and create the store with
    // correct dominance.
    for (auto &[origVal, commits] : pendingSSACommits) {
        llvm::SmallVector<llvm::PHINode *, 4> newPHIs;
        llvm::SSAUpdater commitUpdater(&newPHIs);
        commitUpdater.Initialize(origVal->getType(), origVal->getName());

        // Seed only the original definition.  Ineligible SSA values are
        // always Instructions (identified by iterating over block
        // instructions in StateAnalysis).
        auto *I = llvm::dyn_cast<llvm::Instruction>(origVal);
        assert(I && "SSA commit value must be an Instruction");
        commitUpdater.AddAvailableValue(I->getParent(), origVal);

        for (auto &[nvmBackup, insertBefore] : commits) {
            // GetValueInMiddleOfBlock is correct here: commit stores sit
            // at the top of the boundary block (before the boundary call).
            // When the defining block *is* the boundary, GVIMOB walks
            // predecessors to find the incoming value — the value from
            // the closing region, which is exactly what we want to commit.
            llvm::Value *reaching =
                commitUpdater.GetValueInMiddleOfBlock(insertBefore->getParent());
            llvm::IRBuilder<> commitBuilder(insertBefore->getParent(), insertBefore->getIterator());
            auto *store = commitBuilder.CreateStore(reaching, nvmBackup);
            allCommitInsts.insert(store);

            if (addDebugMarkers_)
                emitCounterIncrement(commitBuilder, cntSaveVregGV_);
        }

        // Mark new PHIs as commit-related so Phase 3 restore rewriting
        // skips them.
        for (auto *PHI : newPHIs)
            allCommitInsts.insert(PHI);
    }

    // Phase 3: Use SSAUpdater for each ineligible SSA value to correctly
    // handle multi-definition reaching and insert PHI nodes at merge points.
    for (auto &[origVal, defs] : ssaRestoreDefs) {
        llvm::SSAUpdater updater;
        updater.Initialize(origVal->getType(), origVal->getName());

        // The original definition is available in its defining block.
        auto *defInst = llvm::dyn_cast<llvm::Instruction>(origVal);
        const llvm::BasicBlock *defBlock = nullptr;
        if (defInst) {
            defBlock = defInst->getParent();
            updater.AddAvailableValue(defInst->getParent(), origVal);
        }

        // Each boundary's restore load is available in its block.
        for (auto &[block, restoredVal] : defs)
            updater.AddAvailableValue(block, restoredVal);

        // Collect uses to rewrite (can't modify use-list while iterating).
        llvm::SmallVector<llvm::Use *, 16> usesToRewrite;
        for (auto &U : origVal->uses()) {
            auto *I = llvm::dyn_cast<llvm::Instruction>(U.getUser());
            if (!I)
                continue;
            // Skip commit stores — they correctly use the original value
            // during normal (non-restart) execution.
            if (allCommitInsts.count(I))
                continue;
            // Keep same-block uses on the original SSA def. These uses are
            // dominated by the local definition and should not consume a
            // boundary restore value.
            if (defBlock && I->getParent() == defBlock)
                continue;
            usesToRewrite.push_back(&U);
        }

        // Use RewriteUseAfterInsertions (GetValueAtEndOfBlock) rather than
        // RewriteUse (GetValueInMiddleOfBlock).  GetValueInMiddleOfBlock
        // treats the available value as defined mid-block and walks to
        // predecessors for same-block uses — returning poison for the
        // entry block (no predecessors).  GetValueAtEndOfBlock returns the
        // available value directly, which is correct because all original
        // instructions come after the inserted restore loads.
        for (llvm::Use *U : usesToRewrite)
            updater.RewriteUseAfterInsertions(*U);
    }

    return inserted;
}

} // namespace checkpoint
