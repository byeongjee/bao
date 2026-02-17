#include "milp/CheckpointInstrumenter.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <cassert>
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
    // Skip ineligibles — they don't have placeInVm variables.
    std::set<llvm::GlobalVariable *> vmPlacedGVs;
    for (const auto &[key, placed] : solution.placeInVm) {
        if (placed && !state.isIneligibleGlobal(key.second)) {
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

    // Create NVM backup for every ineligible global accessed in the function.
    // Ineligibles always reside in VM (SRAM); their backup lives in NVM (.nvm).
    for (llvm::GlobalVariable *GV : state.getIneligibleObjs()) {
        auto *backup = new llvm::GlobalVariable(
            M_, GV->getValueType(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(GV->getValueType()),
            "__nvm_backup_" + GV->getName().str());
        backup->setSection(".nvm");
        backup->setAlignment(GV->getAlign());
        nvmBackupMap_[GV] = backup;
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

        // For b != b0, emit boundary-end code first.
        if (!isEntryNode) {
            builder.CreateCall(epilogueFn_);
            inserted++;

            std::set<llvm::GlobalVariable *> commitGVs;
            for (const auto &[key, enabled] : solution.commit) {
                if (!enabled) {
                    continue;
                }
                if (!nodeSet.count(key.first)) {
                    continue;
                }
                commitGVs.insert(key.second);
            }

            for (llvm::GlobalVariable *GV : commitGVs) {
                unsigned sizeBytes = state.getVMObjSizeBytes(GV);
                if (sizeBytes == 0) {
                    continue;
                }
                llvm::Value *size = llvm::ConstantInt::get(
                    llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
                auto shadowIt = shadowMap_.find(GV);
                if (shadowIt != shadowMap_.end()) {
                    // Eligible: copy shadow (VM/SRAM) -> original (NVM/FRAM)
                    builder.CreateCall(storeMemFn_,
                                       {GV, shadowIt->second, size});
                } else {
                    // Ineligible: copy SRAM original -> NVM backup
                    auto backupIt = nvmBackupMap_.find(GV);
                    assert(backupIt != nvmBackupMap_.end() &&
                           "ineligible commit requires an NVM backup");
                    builder.CreateCall(storeMemFn_,
                                       {backupIt->second, GV, size});
                }
                inserted++;
            }
        }

        // Then emit boundary-start code.
        builder.CreateCall(prologueFn_);
        inserted++;

        std::set<llvm::GlobalVariable *> restoreGVs;
        for (const auto &[key, enabled] : solution.needRestore) {
            if (!enabled) {
                continue;
            }
            if (!nodeSet.count(key.first)) {
                continue;
            }
            restoreGVs.insert(key.second);
        }

        for (llvm::GlobalVariable *GV : restoreGVs) {
            unsigned sizeBytes = state.getVMObjSizeBytes(GV);
            if (sizeBytes == 0) {
                continue;
            }
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
        // the global is live-in. No needRestore variable — always restore.
        for (llvm::GlobalVariable *GV : state.getIneligibleObjs()) {
            auto backupIt = nvmBackupMap_.find(GV);
            if (backupIt == nvmBackupMap_.end())
                continue;
            // Check if GV is live-in at any node mapped to this block.
            bool isLiveIn = false;
            for (NodeId node : nodes) {
                if (stateView.getVMObjLiveIn(node).count(GV)) {
                    isLiveIn = true;
                    break;
                }
            }
            if (!isLiveIn)
                continue;
            unsigned sizeBytes = state.getVMObjSizeBytes(GV);
            if (sizeBytes == 0)
                continue;
            llvm::Value *size = llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(M_.getContext()), sizeBytes);
            // Restore: copy NVM backup -> SRAM original
            builder.CreateCall(restoreMemFn_,
                               {GV, backupIt->second, size});
            inserted++;
        }
    }

    return inserted;
}

} // namespace checkpoint
