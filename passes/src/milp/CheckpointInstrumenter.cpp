#include "milp/CheckpointInstrumenter.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

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
    const StateAnalysis &state) {

    unsigned inserted = 0;
    inserted += insertRegionBoundaries(F, solution, cfg, state);
    applyMemoryPlacement(state);
    return inserted;
}

unsigned CheckpointInstrumenter::insertRegionBoundaries(
    llvm::Function &F,
    const MILPSolution &solution,
    const ICFGView &cfg,
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
                builder.CreateCall(storeMemFn_, {GV, GV, size});
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
            builder.CreateCall(restoreMemFn_, {GV, GV, size});
            inserted++;
        }
    }

    return inserted;
}

void CheckpointInstrumenter::applyMemoryPlacement(const StateAnalysis &state) {
    for (llvm::GlobalVariable *GV : state.getVMObjs()) {
        GV->setSection(".candidate");
    }
}

} // namespace checkpoint
