#include "milp/CheckpointInstrumenter.h"
#include "common/BlockUtils.h"
#include "common/Logger.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
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
    for (const auto &[key, placed] : solution.m) {
        if (!placed)
            continue;
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(key.second))
            vmPlacedGVs.insert(GV);
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
            auto key = std::make_pair(nodeId, static_cast<llvm::Value *>(GV));
            auto pvIt = solution.m.find(key);
            if (pvIt == solution.m.end() || !pvIt->second)
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

            std::set<llvm::Value *> commitVars;
            for (NodeId n : nodeSet)
                for (llvm::Value *V : solution.getSaveVarsAt(n))
                    commitVars.insert(V);

            // Ineligible live-in values must also be committed so that the
            // restore loads after the boundary read correct values.  The MILP
            // solver only tracks eligible (global) objects in its save set;
            // ineligible SSA values and allocas are handled here.
            for (llvm::Value *V : state.getIneligLiveIn(&BB))
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
                    builder.CreateMemCpy(GV, GV->getAlign(), shadowIt->second,
                                         shadowIt->second->getAlign(), size);
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntStoreMemGV_);
                } else if (llvm::isa<llvm::AllocaInst>(V)) {
                    llvm::Value *size =
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() &&
                           "alloca commit requires an NVM backup");
                    auto *AI = llvm::cast<llvm::AllocaInst>(V);
                    builder.CreateMemCpy(backupIt->second, backupIt->second->getAlign(), AI,
                                         AI->getAlign(), size);
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntStoreMemGV_);
                } else {
                    // SSA value: unified commit via GetValueInMiddleOfBlock.
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() && "SSA commit requires an NVM backup");
                    auto *defInst = llvm::cast<llvm::Instruction>(V);
                    llvm::SSAUpdater commitUpdater;
                    commitUpdater.Initialize(V->getType(), "ssa.commit");
                    commitUpdater.AddAvailableValue(defInst->getParent(), V);
                    llvm::Value *reachingVal = commitUpdater.GetValueInMiddleOfBlock(&BB);
                    builder.CreateStore(reachingVal, backupIt->second);
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntSaveVregGV_);
                }
                inserted++;
            }

            // Emit boundary call.
            auto *boundaryCall = builder.CreateCall(boundaryFn_);
            inserted++;

            // --- Split block after boundary call ---
            // BB_bottom receives all instructions that were after the split point
            // (the original block's remaining code). Restores go into BB_bottom.
            llvm::BasicBlock *BB_bottom = llvm::SplitBlock(&BB, boundaryCall->getNextNode());
            builder.SetInsertPoint(&*BB_bottom->getFirstInsertionPt());
        }
        // Entry node: no boundary call, no split. Restores still emitted below.

        // --- Restore phase (in BB_bottom for non-entry, in BB for entry) ---
        // The builder is now positioned in the restore block.

        // Eligible restores (needRestore).
        std::set<llvm::GlobalVariable *> restoreGVs;
        for (NodeId n : nodeSet) {
            for (llvm::Value *V : solution.getRestoreVarsAt(n)) {
                if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
                    restoreGVs.insert(GV);
            }
        }

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
            if (addDebugMarkers_)
                emitCounterIncrement(builder, cntRestoreMemGV_);
            inserted++;
        }

        // Ineligible restores: unconditional at every region start where
        // the object is live-in.
        for (llvm::Value *V : state.getIneligLiveIn(&BB)) {
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
                builder.CreateMemCpy(AI, AI->getAlign(), backupIt->second,
                                     backupIt->second->getAlign(), size);
                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntRestoreMemGV_);
                inserted++;
            } else {
                // SSA restore: load from NVM backup, record for SSAUpdater.
                llvm::Value *restored = builder.CreateLoad(V->getType(), backupIt->second);
                ssaRestoreDefs[V].emplace_back(builder.GetInsertBlock(), restored);
                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntRestoreVregGV_);
                inserted++;
            }
        }
    }

    // SSAUpdater pass: rewrite uses of SSA values to pick up restore loads.
    // Each SSA value V has available definitions:
    //   - V itself in its def block (original definition)
    //   - restored value in each BB_bottom (after boundary)
    // SSAUpdater selects the correct reaching definition for each use.
    for (auto &[origVal, defs] : ssaRestoreDefs) {
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
