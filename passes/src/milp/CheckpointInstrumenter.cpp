#include "milp/CheckpointInstrumenter.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
#include <queue>
#include <set>
#include <vector>

namespace checkpoint {

CheckpointInstrumenter::CheckpointInstrumenter(llvm::Module &M) : M_(M) {
    declareRuntimeFunctions();
}

void CheckpointInstrumenter::declareRuntimeFunctions() {
    llvm::LLVMContext &Ctx = M_.getContext();
    llvm::Type *VoidTy = llvm::Type::getVoidTy(Ctx);
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
    llvm::Type *PtrTy = llvm::PointerType::get(Ctx, 0);

    prologueFn_ = M_.getOrInsertFunction("__region_prologue", VoidTy);
    epilogueFn_ = M_.getOrInsertFunction("__region_epilogue", VoidTy);
    storeMemFn_ = M_.getOrInsertFunction(
        "__checkpoint_store_mem", VoidTy, PtrTy, PtrTy, I32Ty);
    restoreMemFn_ = M_.getOrInsertFunction(
        "__restore_mem", VoidTy, PtrTy, PtrTy, I32Ty);
}

unsigned CheckpointInstrumenter::instrumentFunction(
    llvm::Function &F,
    const MILPSolution &solution,
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
    for (llvm::GlobalVariable *GV : state.getVMObjs()) {
        GV->setSection(".nvm");
    }
}

void CheckpointInstrumenter::createShadowGlobals(
    llvm::Function &F,
    const MILPSolution &solution,
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
            M_, GV->getValueType(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(GV->getValueType()),
            "__vm_shadow_" + GV->getName().str());
        shadow->setAlignment(GV->getAlign());
        shadowMap_[GV] = shadow;
    }
}

void CheckpointInstrumenter::createNVMBackupGlobals(
    llvm::Function &F,
    const MILPSolution &solution,
    const StateAnalysis &state,
    const ICFGView &cfg) {

    nvmBackupMap_.clear();

    unsigned ssaCounter = 0;
    for (llvm::Value *V : state.getIneligibleObjs()) {
        llvm::Type *backupType = nullptr;
        std::string backupName;

        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
            backupType = GV->getValueType();
            backupName = "__nvm_backup_" + GV->getName().str();
        } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V)) {
            backupType = AI->getAllocatedType();
            backupName = "__nvm_alloca_" + (AI->hasName()
                             ? AI->getName().str()
                             : std::to_string(ssaCounter++));
        } else if (auto *Inst = llvm::dyn_cast<llvm::Instruction>(V)) {
            backupType = Inst->getType();
            backupName = "__nvm_ssa_" + std::to_string(ssaCounter++);
        } else {
            continue;
        }

        auto *backup = new llvm::GlobalVariable(
            M_, backupType, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(backupType),
            backupName);
        backup->setSection(".nvm");
        nvmBackupMap_[V] = backup;
    }
}

void CheckpointInstrumenter::rewriteAccessesInVMRegions(
    llvm::Function &F,
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
            for (llvm::Instruction &I : BB) {
                for (unsigned i = 0; i < I.getNumOperands(); ++i) {
                    if (I.getOperand(i) == GV) {
                        I.setOperand(i, shadow);
                    }
                }
            }
        }
    }
}

std::set<llvm::BasicBlock *> CheckpointInstrumenter::computeRegionBlocks(
    llvm::BasicBlock *start,
    const MILPSolution &solution,
    const ICFGView &cfg) const {

    std::set<llvm::BasicBlock *> region;
    std::queue<llvm::BasicBlock *> worklist;
    worklist.push(start);
    region.insert(start);

    while (!worklist.empty()) {
        llvm::BasicBlock *BB = worklist.front();
        worklist.pop();
        for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
            if (region.count(Succ))
                continue;
            // Stop at other region starts (don't include them).
            NodeId succId = cfg.getNodeMap().getNodeId(Succ);
            if (succId != kInvalidNodeId &&
                solution.regionStarts.count(succId) &&
                Succ != start)
                continue;
            region.insert(Succ);
            worklist.push(Succ);
        }
    }
    return region;
}

unsigned CheckpointInstrumenter::insertRegionBoundaries(
    llvm::Function &F,
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

    for (llvm::BasicBlock &BB : F) {
        auto itNodeVec = nodesByBlock.find(&BB);
        if (itNodeVec == nodesByBlock.end()) {
            continue;
        }

        const std::vector<NodeId> &nodes = itNodeVec->second;
        std::set<NodeId> nodeSet(nodes.begin(), nodes.end());

        llvm::BasicBlock::iterator insertIt = BB.getFirstNonPHIIt();
        if (insertIt == BB.end()) {
            continue;
        }

        llvm::IRBuilder<> builder(&BB, insertIt);

        bool isEntryNode = nodeSet.count(entryNode) > 0;

        // Track commit instructions so SSA use rewriting skips them.
        std::set<llvm::Instruction *> commitInsts;

        // For b != b0, emit boundary-end code first.
        // Order: commit stores -> epilogue -> prologue -> restores
        if (!isEntryNode) {
            // Commit dirty data while the region is still active.
            std::set<llvm::Value *> commitVars;
            for (const auto &[key, enabled] : solution.commit) {
                if (!enabled)
                    continue;
                if (!nodeSet.count(key.first))
                    continue;
                commitVars.insert(key.second);
            }

            for (llvm::Value *V : commitVars) {
                unsigned sizeBytes = state.getVarSizeBytes(V);
                if (sizeBytes == 0)
                    continue;

                if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
                    llvm::Value *size = llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                    auto shadowIt = shadowMap_.find(GV);
                    if (shadowIt != shadowMap_.end()) {
                        // Eligible: copy shadow (VM/SRAM) -> original (NVM/FRAM)
                        auto *call = builder.CreateCall(storeMemFn_,
                                           {GV, shadowIt->second, size});
                        commitInsts.insert(call);
                    } else {
                        // Ineligible global: copy SRAM original -> NVM backup
                        auto backupIt = nvmBackupMap_.find(GV);
                        assert(backupIt != nvmBackupMap_.end() &&
                               "ineligible commit requires an NVM backup");
                        auto *call = builder.CreateCall(storeMemFn_,
                                           {backupIt->second, GV, size});
                        commitInsts.insert(call);
                    }
                } else if (llvm::isa<llvm::AllocaInst>(V)) {
                    // Stack alloca -> NVM backup (memcpy)
                    llvm::Value *size = llvm::ConstantInt::get(
                        llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() &&
                           "alloca commit requires an NVM backup");
                    auto *call = builder.CreateCall(storeMemFn_,
                                       {backupIt->second, V, size});
                    commitInsts.insert(call);
                } else {
                    // SSA register -> NVM slot (typed store)
                    auto backupIt = nvmBackupMap_.find(V);
                    assert(backupIt != nvmBackupMap_.end() &&
                           "SSA commit requires an NVM backup");
                    auto *store = builder.CreateStore(V, backupIt->second);
                    commitInsts.insert(store);
                }
                inserted++;
            }

            // Finalize the old region after all commits are done.
            builder.CreateCall(epilogueFn_);
            inserted++;
        }

        // Then emit boundary-start code.
        builder.CreateCall(prologueFn_);
        inserted++;

        // Eligible restores (needRestore).
        std::set<llvm::GlobalVariable *> restoreGVs;
        for (const auto &[key, enabled] : solution.needRestore) {
            if (!enabled)
                continue;
            if (!nodeSet.count(key.first))
                continue;
            restoreGVs.insert(key.second);
        }

        for (llvm::GlobalVariable *GV : restoreGVs) {
            unsigned sizeBytes = state.getVarSizeBytes(GV);
            if (sizeBytes == 0)
                continue;
            llvm::Value *size = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
            // Restore: copy original (NVM/FRAM) -> shadow (VM/SRAM)
            auto shadowIt = shadowMap_.find(GV);
            assert(shadowIt != shadowMap_.end() &&
                   "needRestore requires a VM shadow for the global");
            builder.CreateCall(restoreMemFn_,
                               {shadowIt->second, GV, size});
            inserted++;
        }

        // Ineligible restores: unconditional at every region start where
        // the object is live-in.
        std::map<llvm::Value *, llvm::Value *> ssaRestoreMap;
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

            if (llvm::isa<llvm::GlobalVariable>(V) ||
                llvm::isa<llvm::AllocaInst>(V)) {
                // Memory object: memcpy from NVM backup.
                unsigned sizeBytes = state.getVarSizeBytes(V);
                if (sizeBytes == 0)
                    continue;
                llvm::Value *size = llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                // Restore: copy NVM backup -> SRAM original
                builder.CreateCall(restoreMemFn_,
                                   {V, backupIt->second, size});
                inserted++;
            } else {
                // SSA register: typed load from NVM slot.
                llvm::Value *restored =
                    builder.CreateLoad(V->getType(), backupIt->second);
                ssaRestoreMap[V] = restored;
                inserted++;
            }
        }

        // SSA use rewriting: replace uses of original SSA values with restored
        // values within this region, but NOT in commit stores.
        if (!ssaRestoreMap.empty()) {
            std::set<llvm::BasicBlock *> regionBlocks =
                computeRegionBlocks(&BB, solution, cfg);

            for (auto &[origVal, restoredVal] : ssaRestoreMap) {
                origVal->replaceUsesWithIf(restoredVal, [&](llvm::Use &U) {
                    auto *I = llvm::dyn_cast<llvm::Instruction>(U.getUser());
                    if (!I)
                        return false;
                    // Don't replace uses in our commit stores.
                    if (commitInsts.count(I))
                        return false;
                    // For PHI nodes, the relevant block is the incoming block
                    // (where the value must be live), not the PHI's parent.
                    if (auto *PHI = llvm::dyn_cast<llvm::PHINode>(I)) {
                        llvm::BasicBlock *incomingBB =
                            PHI->getIncomingBlock(U);
                        return regionBlocks.count(incomingBB) != 0;
                    }
                    return regionBlocks.count(I->getParent()) != 0;
                });
            }
        }
    }

    return inserted;
}

} // namespace checkpoint
