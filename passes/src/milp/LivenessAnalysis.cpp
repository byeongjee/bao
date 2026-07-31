#include "milp/LivenessAnalysis.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

#include <cassert>
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
    const llvm::DataLayout &DL = F.getParent()->getDataLayout();
    for (llvm::BasicBlock &BB : F) {
        for (llvm::GlobalVariable *GV : vmObjs) {
            auto key = std::make_pair(static_cast<const llvm::BasicBlock *>(&BB), GV);
            BlockGVInfo info;
            bool seenMustStore = false;
            uint64_t gvBytes = DL.getTypeAllocSize(GV->getValueType()).getFixedValue();

            for (llvm::Instruction &I : BB) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA.getModRefInfo(&I, Loc);

                if (llvm::isRefSet(MRI) && !seenMustStore)
                    info.loadBeforeMustStore = true;

                // A store kills liveness only when it provably overwrites the
                // entire global. An element store like g[0] = x folds its
                // all-zero GEP away and looks like a store to the global
                // itself, but only covers part of an aggregate.
                if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                    llvm::Value *Ptr = SI->getPointerOperand()->stripPointerCasts();
                    if (Ptr == GV &&
                        DL.getTypeStoreSize(SI->getValueOperand()->getType()).getFixedValue() >=
                            gvBytes) {
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
                auto infoIt = blockGVInfo.find(std::make_pair(BB, GV));
                assert(infoIt != blockGVInfo.end() &&
                       "phase 1 scanned a different block set than cfg.getBlocks()");
                const BlockGVInfo &info = infoIt->second;

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
computeIneligAllocaLiveness(llvm::Function &F, llvm::AAResults &AA, const CFGAnalysis &cfg,
                            const std::vector<llvm::Value *> &ineligibleObjs) {

    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>> result;

    std::vector<llvm::Value *> allocaIneligs;
    for (llvm::Value *V : ineligibleObjs) {
        if (llvm::isa<llvm::AllocaInst>(V))
            allocaIneligs.push_back(V);
    }

    if (allocaIneligs.empty())
        return result;

    struct BlockVarInfo {
        bool loadBeforeMustStore = false;
        bool hasMustStore = false;
    };

    std::map<std::pair<const llvm::BasicBlock *, llvm::Value *>, BlockVarInfo> blockVarInfo;

    // Phase 1: Per-instruction scan (iterate F for non-const access).
    for (llvm::BasicBlock &BB : F) {
        for (llvm::Value *V : allocaIneligs) {
            auto key = std::make_pair(static_cast<const llvm::BasicBlock *>(&BB), V);
            BlockVarInfo info;
            bool seenMustStore = false;

            auto *AI = llvm::cast<llvm::AllocaInst>(V);
            const llvm::DataLayout &DL = F.getParent()->getDataLayout();
            auto allocaBytes = AI->getAllocationSize(DL);
            for (llvm::Instruction &I : BB) {
                // Reads are detected via alias analysis so accesses through
                // GEPs (array elements, struct fields) are visible.  A
                // write counts as a must-store only when it provably
                // overwrites the entire alloca: a direct store of a value
                // covering the full size, or a mem intrinsic writing at
                // least the full size.  Partial writes (element stores,
                // partial memsets) must not kill liveness of the rest.
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(AI);
                llvm::ModRefInfo MRI = AA.getModRefInfo(&I, Loc);

                if (llvm::isRefSet(MRI) && !seenMustStore)
                    info.loadBeforeMustStore = true;

                if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                    if (SI->getPointerOperand()->stripPointerCasts() == AI && allocaBytes &&
                        DL.getTypeStoreSize(SI->getValueOperand()->getType()).getFixedValue() >=
                            allocaBytes->getFixedValue()) {
                        info.hasMustStore = true;
                        seenMustStore = true;
                    }
                }
                if (auto *MI = llvm::dyn_cast<llvm::MemIntrinsic>(&I)) {
                    auto *LenCI = llvm::dyn_cast<llvm::ConstantInt>(MI->getLength());
                    if (MI->getRawDest()->stripPointerCasts() == AI && allocaBytes && LenCI &&
                        LenCI->getZExtValue() >= allocaBytes->getFixedValue()) {
                        info.hasMustStore = true;
                        seenMustStore = true;
                    }
                }
            }

            blockVarInfo[key] = info;
        }
    }

    // Phase 2: Dataflow iteration.
    for (llvm::Value *V : allocaIneligs) {
        llvm::DenseMap<const llvm::BasicBlock *, bool> liveIn, liveOut;
        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            liveIn[BB] = false;
            liveOut[BB] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
                auto infoIt = blockVarInfo.find(std::make_pair(BB, V));
                assert(infoIt != blockVarInfo.end() &&
                       "phase 1 scanned a different block set than cfg.getBlocks()");
                const BlockVarInfo &info = infoIt->second;

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

        if (useBlocks.empty() && !llvm::isa<llvm::PHINode>(Inst))
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
