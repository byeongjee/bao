#include "milp/LivenessAnalysis.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

#include <map>

namespace checkpoint {

llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::GlobalVariable *>>
computeEligibleLiveness(llvm::Function &F, llvm::AAResults &AA, const CFGAnalysis &cfg,
                        const std::vector<llvm::GlobalVariable *> &vmObjs) {

    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> result;

    if (vmObjs.empty())
        return result;

    struct BlockGVInfo {
        bool loadBeforeMustStore = false;
        bool hasMustStore = false;
    };

    std::map<std::pair<const llvm::BasicBlock *, llvm::GlobalVariable *>, BlockGVInfo> blockGVInfo;

    // Phase 1: Per-instruction scan (iterate F for non-const access).
    for (llvm::BasicBlock &BB : F) {
        for (llvm::GlobalVariable *GV : vmObjs) {
            auto key = std::make_pair(static_cast<const llvm::BasicBlock *>(&BB), GV);
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

    // Phase 2: Dataflow iteration.
    for (llvm::GlobalVariable *GV : vmObjs) {
        llvm::DenseMap<const llvm::BasicBlock *, bool> liveIn, liveOut;
        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            liveIn[BB] = false;
            liveOut[BB] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
                auto key = std::make_pair(BB, GV);
                const BlockGVInfo &info = blockGVInfo[key];

                bool newLiveOut = false;
                for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    if (liveIn[Succ]) {
                        newLiveOut = true;
                        break;
                    }
                }

                bool newLiveIn = info.loadBeforeMustStore || (newLiveOut && !info.hasMustStore);

                if (newLiveIn != liveIn[BB] || newLiveOut != liveOut[BB]) {
                    liveIn[BB] = newLiveIn;
                    liveOut[BB] = newLiveOut;
                    changed = true;
                }
            }
        }

        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            if (liveIn[BB])
                result[BB].insert(GV);
        }
    }

    return result;
}

llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>>
computeIneligGlobalAllocaLiveness(llvm::Function &F, llvm::AAResults &AA, const CFGAnalysis &cfg,
                                  const std::vector<llvm::Value *> &ineligibleObjs) {

    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>> result;

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

    std::map<std::pair<const llvm::BasicBlock *, llvm::Value *>, BlockVarInfo> blockVarInfo;

    // Phase 1: Per-instruction scan (iterate F for non-const access).
    for (llvm::BasicBlock &BB : F) {
        for (llvm::Value *V : globalAllocaIneligs) {
            auto key = std::make_pair(static_cast<const llvm::BasicBlock *>(&BB), V);
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
                        if (LI->getPointerOperand()->stripPointerCasts() == AI && !seenMustStore)
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

    // Phase 2: Dataflow iteration.
    for (llvm::Value *V : globalAllocaIneligs) {
        llvm::DenseMap<const llvm::BasicBlock *, bool> liveIn, liveOut;
        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            liveIn[BB] = false;
            liveOut[BB] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
                auto key = std::make_pair(BB, V);
                const BlockVarInfo &info = blockVarInfo[key];

                bool newLiveOut = false;
                for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    if (liveIn[Succ]) {
                        newLiveOut = true;
                        break;
                    }
                }

                bool newLiveIn = info.loadBeforeMustStore || (newLiveOut && !info.hasMustStore);

                if (newLiveIn != liveIn[BB] || newLiveOut != liveOut[BB]) {
                    liveIn[BB] = newLiveIn;
                    liveOut[BB] = newLiveOut;
                    changed = true;
                }
            }
        }

        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            if (liveIn[BB])
                result[BB].insert(V);
        }
    }

    return result;
}

llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>>
computeIneligSSALiveness(llvm::Function &F, const CFGAnalysis &cfg,
                         const std::vector<llvm::Value *> &ineligibleObjs) {

    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>> result;

    for (llvm::Value *V : ineligibleObjs) {
        auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
        if (!Inst || llvm::isa<llvm::AllocaInst>(Inst))
            continue;

        const llvm::BasicBlock *defBlock = Inst->getParent();

        // Collect PHI incoming edges and non-PHI use blocks.
        // phiIncoming: PHI_block -> {incoming blocks that carry V}
        llvm::DenseMap<const llvm::BasicBlock *, llvm::SmallPtrSet<const llvm::BasicBlock *, 4>>
            phiIncoming;
        llvm::SmallPtrSet<const llvm::BasicBlock *, 8> useBlocks;
        for (const llvm::User *U : Inst->users()) {
            if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
                if (auto *PHI = llvm::dyn_cast<llvm::PHINode>(UI)) {
                    const llvm::BasicBlock *phiBlock = PHI->getParent();
                    for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
                        if (PHI->getIncomingValue(i) == Inst) {
                            const llvm::BasicBlock *incBB = PHI->getIncomingBlock(i);
                            phiIncoming[phiBlock].insert(incBB);
                        }
                    }
                } else if (UI->getParent() != defBlock) {
                    useBlocks.insert(UI->getParent());
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
        llvm::DenseMap<const llvm::BasicBlock *, bool> realLiveIn, liveOut;
        llvm::SmallPtrSet<const llvm::BasicBlock *, 8> phiLiveInBlocks;
        for (const auto &[phiBlock, _] : phiIncoming)
            phiLiveInBlocks.insert(phiBlock);

        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            realLiveIn[BB] = false;
            liveOut[BB] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
                if (BB == defBlock)
                    continue;

                bool newLiveOut = false;
                for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    if (realLiveIn[Succ]) {
                        newLiveOut = true;
                        break;
                    }
                    // PHI edge: only propagate if PHI at Succ
                    // receives V from this block.
                    auto it = phiIncoming.find(Succ);
                    if (it != phiIncoming.end() && it->second.count(BB)) {
                        newLiveOut = true;
                        break;
                    }
                }

                bool isUsed = useBlocks.count(BB) > 0;
                bool newRealLiveIn = isUsed || newLiveOut;

                if (newRealLiveIn != realLiveIn[BB] || newLiveOut != liveOut[BB]) {
                    realLiveIn[BB] = newRealLiveIn;
                    liveOut[BB] = newLiveOut;
                    changed = true;
                }
            }
        }

        // V is live-in if it has real or PHI liveness.
        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            if (realLiveIn[BB] || phiLiveInBlocks.count(BB))
                result[BB].insert(V);
        }
    }

    return result;
}

} // namespace checkpoint
