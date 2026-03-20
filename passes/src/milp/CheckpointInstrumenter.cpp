#include "milp/CheckpointInstrumenter.h"
#include "common/BlockUtils.h"
#include "common/Logger.h"

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
    // rewriting (non-PHI SSA values only).
    std::map<llvm::Value *, std::vector<std::pair<llvm::BasicBlock *, llvm::Value *>>>
        ssaRestoreDefs;
    // Track all commit-related instructions so SSAUpdater skips them.
    std::set<llvm::Instruction *> allCommitInsts;

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

        // For b != b0, emit: commit stores -> region boundary -> restores
        if (!isEntryNode) {
            // Step 1: Build PHI map — maps incoming values to their PHI nodes.
            // When LCSSA/strip-mining produces PHIs at boundary blocks, commit
            // variables from StateAnalysis arrive through these PHIs. We commit
            // the PHI itself (the resolved value at the boundary) rather than
            // the original def which may not dominate this point.
            llvm::DenseMap<llvm::Value *, llvm::SmallVector<llvm::PHINode *, 2>> phiMap;
            for (auto &PHI : BB.phis()) {
                for (unsigned i = 0; i < PHI.getNumIncomingValues(); ++i)
                    phiMap[PHI.getIncomingValue(i)].push_back(&PHI);
            }

            // SSA commits split into two categories:
            // - PHI commits: committed value is a PHI in this block; restore
            //   uses replaceUsesWithIf (PHI dominates all its uses, so the
            //   restore load in the same block also dominates them).
            // - Non-PHI commits: committed value is the original V which
            //   dominates this block from elsewhere; restore uses SSAUpdater
            //   (handles multi-block domination correctly).
            struct PHICommitRecord {
                llvm::SmallVector<llvm::PHINode *, 2> phis;
                llvm::StoreInst *commitStore;
                llvm::GlobalVariable *nvmBackup;
            };
            std::vector<PHICommitRecord> phiCommits;

            struct NonPHICommitRecord {
                llvm::Value *origVal;
                llvm::GlobalVariable *nvmBackup;
            };
            std::vector<NonPHICommitRecord> nonPhiCommits;

            // Step 2: Emit commit stores.
            std::set<llvm::Value *> commitVars;
            for (NodeId n : nodeSet)
                for (llvm::Value *V : solution.getSaveVarsAt(n))
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
                    auto *mc = builder.CreateMemCpy(GV, GV->getAlign(), shadowIt->second,
                                                    shadowIt->second->getAlign(), size);
                    allCommitInsts.insert(mc);
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntStoreMemGV_);
                } else if (llvm::isa<llvm::AllocaInst>(V)) {
                    llvm::Value *size =
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() &&
                           "alloca commit requires an NVM backup");
                    auto *AI = llvm::cast<llvm::AllocaInst>(V);
                    auto *mc = builder.CreateMemCpy(backupIt->second, backupIt->second->getAlign(),
                                                    AI, AI->getAlign(), size);
                    allCommitInsts.insert(mc);
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntStoreMemGV_);
                } else {
                    // SSA value: commit through PHI if one exists.
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() && "SSA commit requires an NVM backup");
                    auto phiIt = phiMap.find(V);
                    if (phiIt != phiMap.end()) {
                        // PHI case: store through the first PHI (all capture
                        // the same runtime value of V from the same predecessor).
                        // Record all PHIs so restore rewrites uses of each one.
                        auto *commitStore =
                            builder.CreateStore(phiIt->second.front(), backupIt->second);
                        allCommitInsts.insert(commitStore);
                        phiCommits.push_back({phiIt->second, commitStore, backupIt->second});
                    } else {
                        // Non-PHI case: resolve V's reaching definition at BB.
                        // V may not dominate BB (e.g. at a merge point where V
                        // was only computed on some paths).  SSAUpdater returns
                        // V itself when V dominates BB, or inserts a PHI that
                        // merges V with undef on non-dominating paths.  The
                        // undef is safe: on those paths V was never computed,
                        // so the NVM backup retains its prior valid value and
                        // the stored undef is never read.
                        auto *defInst = llvm::cast<llvm::Instruction>(V);
                        llvm::SSAUpdater commitUpdater;
                        commitUpdater.Initialize(V->getType(), "ssa.commit");
                        commitUpdater.AddAvailableValue(defInst->getParent(), V);
                        llvm::Value *reachingVal = commitUpdater.GetValueInMiddleOfBlock(&BB);

                        // Protect any inserted PHI from the restore SSAUpdater.
                        if (reachingVal != V) {
                            if (auto *PHI = llvm::dyn_cast<llvm::PHINode>(reachingVal))
                                allCommitInsts.insert(PHI);
                        }

                        auto *commitStore = builder.CreateStore(reachingVal, backupIt->second);
                        allCommitInsts.insert(commitStore);
                        nonPhiCommits.push_back({V, backupIt->second});
                    }
                    if (addDebugMarkers_)
                        emitCounterIncrement(builder, cntSaveVregGV_);
                }
                inserted++;
            }

            // Step 3: Emit boundary call.
            builder.CreateCall(boundaryFn_);
            inserted++;

            // Step 4a: PHI restores — use replaceUsesWithIf.
            // Each PHI is defined in BB, so the restore load (also in BB, after
            // the boundary call) dominates all PHI uses. Safe to replace directly.
            // When multiple PHIs capture the same incoming value, we emit one
            // restore load and rewrite uses of every such PHI.
            for (auto &rec : phiCommits) {
                llvm::Value *restoreVal =
                    builder.CreateLoad(rec.phis.front()->getType(), rec.nvmBackup);
                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntRestoreVregGV_);

                for (llvm::PHINode *phi : rec.phis) {
                    phi->replaceUsesWithIf(restoreVal, [&allCommitInsts](llvm::Use &U) {
                        auto *I = llvm::dyn_cast<llvm::Instruction>(U.getUser());
                        return !I || !allCommitInsts.count(I);
                    });
                }
                inserted++;
            }

            // Non-PHI commits: no restore here.  The commit store writes the
            // value to NVM; the ineligible restore path (below) handles
            // restoration for values that are actually live-in at the
            // boundary, using the same NVM slots via nvmBackupMap_.
        }
        // Entry node: no boundary call at program start.

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
        for (llvm::Value *V : state.getIneligibleObjs()) {
            auto backupIt = nvmBackupMap_.find(V);
            if (backupIt == nvmBackupMap_.end())
                continue;

            if (!state.getIneligLiveIn(&BB).count(V))
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
                llvm::Value *restored = builder.CreateLoad(V->getType(), backupIt->second);
                ssaRestoreDefs[V].emplace_back(&BB, restored);
                if (addDebugMarkers_)
                    emitCounterIncrement(builder, cntRestoreVregGV_);
                inserted++;
            }
        }
    }

    // SSAUpdater pass: rewrite uses of non-PHI SSA values to pick up
    // restore loads at boundary blocks. This handles values defined in
    // one block with uses in blocks not dominated by the boundary.
    for (auto &[origVal, defs] : ssaRestoreDefs) {
        llvm::SSAUpdater updater;
        updater.Initialize(origVal->getType(), origVal->getName());

        auto *defInst = llvm::dyn_cast<llvm::Instruction>(origVal);
        const llvm::BasicBlock *defBlock = nullptr;
        if (defInst) {
            defBlock = defInst->getParent();
            updater.AddAvailableValue(defInst->getParent(), origVal);
        }

        for (auto &[block, restoredVal] : defs)
            updater.AddAvailableValue(block, restoredVal);

        llvm::SmallVector<llvm::Use *, 16> usesToRewrite;
        for (auto &U : origVal->uses()) {
            auto *I = llvm::dyn_cast<llvm::Instruction>(U.getUser());
            if (!I)
                continue;
            if (allCommitInsts.count(I))
                continue;
            if (defBlock && I->getParent() == defBlock)
                continue;
            usesToRewrite.push_back(&U);
        }

        for (llvm::Use *U : usesToRewrite)
            updater.RewriteUseAfterInsertions(*U);
    }

    return inserted;
}

} // namespace checkpoint
