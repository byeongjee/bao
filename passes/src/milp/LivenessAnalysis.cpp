#include "milp/LivenessAnalysis.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

namespace checkpoint {

std::map<std::string, std::set<llvm::GlobalVariable *>>
computeEligibleLiveness(
    llvm::Function &F,
    llvm::AAResults &AA,
    const CFGAnalysis &cfg,
    const std::vector<llvm::GlobalVariable *> &vmObjs,
    const std::map<std::string, llvm::BasicBlock *> &nameToBlock) {

    std::map<std::string, std::set<llvm::GlobalVariable *>> result;

    if (vmObjs.empty())
        return result;

    struct BlockGVInfo {
        bool loadBeforeMustStore = false;
        bool hasMustStore = false;
    };

    std::map<std::pair<std::string, llvm::GlobalVariable *>, BlockGVInfo> blockGVInfo;

    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);

        for (llvm::GlobalVariable *GV : vmObjs) {
            auto key = std::make_pair(blockName, GV);
            BlockGVInfo info;
            bool seenMustStore = false;

            for (llvm::Instruction &I : BB) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA.getModRefInfo(&I, Loc);

                if (llvm::isRefSet(MRI) && !seenMustStore)
                    info.loadBeforeMustStore = true;

                if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                    llvm::Value *Ptr = SI->getPointerOperand()->stripPointerCasts();
                    if (Ptr == GV) {
                        info.hasMustStore = true;
                        seenMustStore = true;
                    }
                }
            }

            blockGVInfo[key] = info;
        }
    }

    for (llvm::GlobalVariable *GV : vmObjs) {
        std::map<std::string, bool> liveIn, liveOut;
        for (const auto &blockName : cfg.getBlocks()) {
            liveIn[blockName] = false;
            liveOut[blockName] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &blockName : cfg.getBlocks()) {
                auto key = std::make_pair(blockName, GV);
                const BlockGVInfo &info = blockGVInfo[key];

                bool newLiveOut = false;
                llvm::BasicBlock *BB = nameToBlock.at(blockName);
                for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    std::string succName = getBlockName(*Succ, F);
                    if (liveIn[succName]) {
                        newLiveOut = true;
                        break;
                    }
                }

                bool newLiveIn = info.loadBeforeMustStore ||
                                 (newLiveOut && !info.hasMustStore);

                if (newLiveIn != liveIn[blockName] ||
                    newLiveOut != liveOut[blockName]) {
                    liveIn[blockName] = newLiveIn;
                    liveOut[blockName] = newLiveOut;
                    changed = true;
                }
            }
        }

        for (const auto &blockName : cfg.getBlocks()) {
            if (liveIn[blockName])
                result[blockName].insert(GV);
        }
    }

    return result;
}

std::map<std::string, std::set<llvm::Value *>>
computeIneligGlobalAllocaLiveness(
    llvm::Function &F,
    llvm::AAResults &AA,
    const CFGAnalysis &cfg,
    const std::vector<llvm::Value *> &ineligibleObjs,
    const std::map<std::string, llvm::BasicBlock *> &nameToBlock) {

    std::map<std::string, std::set<llvm::Value *>> result;

    std::vector<llvm::Value *> globalAllocaIneligs;
    for (llvm::Value *V : ineligibleObjs) {
        if (llvm::isa<llvm::GlobalVariable>(V) || llvm::isa<llvm::AllocaInst>(V))
            globalAllocaIneligs.push_back(V);
    }

    if (globalAllocaIneligs.empty())
        return result;

    struct BlockVarInfo {
        bool loadBeforeMustStore = false;
        bool hasMustStore = false;
    };

    std::map<std::pair<std::string, llvm::Value *>, BlockVarInfo> blockVarInfo;

    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);

        for (llvm::Value *V : globalAllocaIneligs) {
            auto key = std::make_pair(blockName, V);
            BlockVarInfo info;
            bool seenMustStore = false;

            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
                for (llvm::Instruction &I : BB) {
                    auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                    llvm::ModRefInfo MRI = AA.getModRefInfo(&I, Loc);

                    if (llvm::isRefSet(MRI) && !seenMustStore)
                        info.loadBeforeMustStore = true;

                    if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                        llvm::Value *Ptr = SI->getPointerOperand()->stripPointerCasts();
                        if (Ptr == GV) {
                            info.hasMustStore = true;
                            seenMustStore = true;
                        }
                    }
                }
            } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V)) {
                for (llvm::Instruction &I : BB) {
                    if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                        if (LI->getPointerOperand()->stripPointerCasts() == AI &&
                            !seenMustStore)
                            info.loadBeforeMustStore = true;
                    }
                    if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                        if (SI->getPointerOperand()->stripPointerCasts() == AI) {
                            info.hasMustStore = true;
                            seenMustStore = true;
                        }
                    }
                }
            }

            blockVarInfo[key] = info;
        }
    }

    for (llvm::Value *V : globalAllocaIneligs) {
        std::map<std::string, bool> liveIn, liveOut;
        for (const auto &blockName : cfg.getBlocks()) {
            liveIn[blockName] = false;
            liveOut[blockName] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &blockName : cfg.getBlocks()) {
                auto key = std::make_pair(blockName, V);
                const BlockVarInfo &info = blockVarInfo[key];

                bool newLiveOut = false;
                llvm::BasicBlock *BB = nameToBlock.at(blockName);
                for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    std::string succName = getBlockName(*Succ, F);
                    if (liveIn[succName]) {
                        newLiveOut = true;
                        break;
                    }
                }

                bool newLiveIn = info.loadBeforeMustStore ||
                                 (newLiveOut && !info.hasMustStore);

                if (newLiveIn != liveIn[blockName] ||
                    newLiveOut != liveOut[blockName]) {
                    liveIn[blockName] = newLiveIn;
                    liveOut[blockName] = newLiveOut;
                    changed = true;
                }
            }
        }

        for (const auto &blockName : cfg.getBlocks()) {
            if (liveIn[blockName])
                result[blockName].insert(V);
        }
    }

    return result;
}

std::map<std::string, std::set<llvm::Value *>>
computeIneligSSALiveness(
    llvm::Function &F,
    const CFGAnalysis &cfg,
    const std::vector<llvm::Value *> &ineligibleObjs,
    const std::map<std::string, llvm::BasicBlock *> &nameToBlock) {

    std::map<std::string, std::set<llvm::Value *>> result;

    for (llvm::Value *V : ineligibleObjs) {
        auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
        if (!Inst || llvm::isa<llvm::AllocaInst>(Inst))
            continue;

        llvm::BasicBlock *defBlock = Inst->getParent();
        std::string defBlockName = getBlockName(*defBlock, F);

        // Collect PHI incoming edges and non-PHI use blocks.
        // phiIncoming: PHI_block -> {incoming blocks that carry V}
        std::map<std::string, std::set<std::string>> phiIncoming;
        std::set<std::string> useBlocks;
        for (const llvm::User *U : Inst->users()) {
            if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
                if (auto *PHI = llvm::dyn_cast<llvm::PHINode>(UI)) {
                    std::string phiBlock =
                        getBlockName(*PHI->getParent(), F);
                    for (unsigned i = 0; i < PHI->getNumIncomingValues();
                         ++i) {
                        if (PHI->getIncomingValue(i) == Inst) {
                            llvm::BasicBlock *incBB =
                                PHI->getIncomingBlock(i);
                            phiIncoming[phiBlock].insert(
                                getBlockName(*incBB, F));
                        }
                    }
                } else if (UI->getParent() != defBlock) {
                    useBlocks.insert(getBlockName(*UI->getParent(), F));
                }
            }
        }

        if (useBlocks.empty() && phiIncoming.empty())
            continue;

        // Edge-aware backward dataflow.
        //   phiLiveIn[S]:  V is live-in at S due to a PHI (doesn't
        //                  propagate backward through all predecessors).
        //   realLiveIn[B]: V is live-in at B due to non-PHI use or
        //                  live-out (propagates backward normally).
        //   liveOut[B]:    V is live-out at B if a successor has
        //                  realLiveIn OR a successor's PHI receives V
        //                  specifically from B.
        std::map<std::string, bool> realLiveIn, liveOut;
        std::set<std::string> phiLiveInBlocks;
        for (const auto &[phiBlock, _] : phiIncoming)
            phiLiveInBlocks.insert(phiBlock);

        for (const auto &blockName : cfg.getBlocks()) {
            realLiveIn[blockName] = false;
            liveOut[blockName] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &blockName : cfg.getBlocks()) {
                if (blockName == defBlockName)
                    continue;

                bool newLiveOut = false;
                llvm::BasicBlock *BB = nameToBlock.at(blockName);
                for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    std::string succName = getBlockName(*Succ, F);
                    if (realLiveIn[succName]) {
                        newLiveOut = true;
                        break;
                    }
                    // PHI edge: only propagate if PHI at Succ
                    // receives V from this block.
                    auto it = phiIncoming.find(succName);
                    if (it != phiIncoming.end() &&
                        it->second.count(blockName)) {
                        newLiveOut = true;
                        break;
                    }
                }

                bool isUsed = useBlocks.count(blockName) > 0;
                bool newRealLiveIn = isUsed || newLiveOut;

                if (newRealLiveIn != realLiveIn[blockName] ||
                    newLiveOut != liveOut[blockName]) {
                    realLiveIn[blockName] = newRealLiveIn;
                    liveOut[blockName] = newLiveOut;
                    changed = true;
                }
            }
        }

        // V is live-in if it has real or PHI liveness.
        for (const auto &blockName : cfg.getBlocks()) {
            if (realLiveIn[blockName] ||
                phiLiveInBlocks.count(blockName))
                result[blockName].insert(V);
        }
    }

    return result;
}

} // namespace checkpoint
