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

        // Collect use blocks.  A PHI in block B that uses V does not
        // count as a use of V in B — the PHI is a separate value whose
        // liveness is computed when it is processed as its own entry.
        // V is used at the incoming block (not the PHI's block).
        llvm::SmallPtrSet<const llvm::BasicBlock *, 8> useBlocks;
        for (const llvm::User *U : Inst->users()) {
            if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
                if (auto *PHI = llvm::dyn_cast<llvm::PHINode>(UI)) {
                    for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
                        if (PHI->getIncomingValue(i) == Inst)
                            useBlocks.insert(PHI->getIncomingBlock(i));
                    }
                } else if (UI->getParent() != defBlock) {
                    useBlocks.insert(UI->getParent());
                }
            }
        }

        if (useBlocks.empty())
            continue;

        // Standard backward dataflow.  V is live-in at B if B has a
        // use of V or any successor of B has V live-in.
        llvm::DenseMap<const llvm::BasicBlock *, bool> liveIn;
        for (const llvm::BasicBlock *BB : cfg.getBlocks())
            liveIn[BB] = false;

        bool changed = true;
        while (changed) {
            changed = false;
            for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
                if (BB == defBlock)
                    continue;
                bool newLiveIn = useBlocks.count(BB) > 0;
                if (!newLiveIn) {
                    for (const llvm::BasicBlock *Succ : llvm::successors(BB)) {
                        if (liveIn[Succ]) {
                            newLiveIn = true;
                            break;
                        }
                    }
                }
                if (newLiveIn != liveIn[BB]) {
                    liveIn[BB] = newLiveIn;
                    changed = true;
                }
            }
        }

        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            if (BB == defBlock) {
                if (llvm::isa<llvm::PHINode>(Inst))
                    result[BB].insert(V);
                continue;
            }
            if (liveIn[BB])
                result[BB].insert(V);
        }
    }

    return result;
}

} // namespace checkpoint
