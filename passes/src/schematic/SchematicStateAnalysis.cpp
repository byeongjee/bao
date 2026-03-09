#include "schematic/SchematicStateAnalysis.h"

#include "common/BlockUtils.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <limits>

namespace checkpoint {

namespace {

static bool isWhitelistedHelperName(llvm::StringRef N) {
    return N.starts_with("__mspabi_") || N.starts_with("__aeabi_") || N.starts_with("__div") ||
           N.starts_with("__udiv") || N.starts_with("__mul") || N.starts_with("__mod");
}

} // namespace

SchematicStateAnalysis::SchematicStateAnalysis(llvm::Function &F, llvm::AAResults &AA,
                                               const CFGAnalysis &cfg)
    : F_(F), AA_(AA), cfg_(cfg) {
    identifyCandidates();
    identifyIneligibleSSAValues();
    computeAccessMaps();
}

unsigned SchematicStateAnalysis::getLoadCount(const llvm::BasicBlock *BB, llvm::Value *v) const {
    auto it = loadCounts_.find(std::make_pair(BB, v));
    return it != loadCounts_.end() ? it->second : 0;
}

unsigned SchematicStateAnalysis::getStoreCount(const llvm::BasicBlock *BB, llvm::Value *v) const {
    auto it = storeCounts_.find(std::make_pair(BB, v));
    return it != storeCounts_.end() ? it->second : 0;
}

unsigned SchematicStateAnalysis::getVarSizeBytes(llvm::Value *v) const {
    auto it = varSizeBytes_.find(v);
    return it != varSizeBytes_.end() ? it->second : 0;
}

void SchematicStateAnalysis::printAnalysisErrors(llvm::raw_ostream &os) const {
    for (const auto &msg : analysisErrors_)
        os << msg << "\n";
}

// ---- Private implementation ----

void SchematicStateAnalysis::identifyCandidates() {
    llvm::Module *M = F_.getParent();
    if (!M)
        return;

    const llvm::DataLayout &DL = M->getDataLayout();

    // Track seen objects to avoid duplicates.
    std::set<llvm::Value *> seen;

    // Scan load/store instructions, collect underlying objects.
    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            const llvm::Value *Ptr = nullptr;
            if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I))
                Ptr = LI->getPointerOperand();
            else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I))
                Ptr = SI->getPointerOperand();
            else
                continue;

            const llvm::Value *Obj = llvm::getUnderlyingObject(Ptr->stripPointerCasts());
            llvm::Value *ObjMut = const_cast<llvm::Value *>(Obj);

            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(ObjMut)) {
                // Structural filters (same as MILP StateAnalysis::identifyVMObjs)
                if (GV->isDeclaration())
                    continue;
                if (GV->isConstant())
                    continue;
                if (GV->getName().starts_with("llvm."))
                    continue;
                if (GV->getName().starts_with("__nvm_"))
                    continue;
                if (GV->getName().starts_with("__vm_shadow_"))
                    continue;
                if (!GV->getValueType()->isSized())
                    continue;
                if (!seen.insert(GV).second)
                    continue;

                candidates_.push_back(GV);
                candidateSet_.insert(GV);
                candidateGlobals_.push_back(GV);
                varSizeBytes_[GV] = DL.getTypeAllocSize(GV->getValueType());

            } else if (auto *AI = llvm::dyn_cast<llvm::AllocaInst>(ObjMut)) {
                // Accept static, sized allocas.
                if (!llvm::isa<llvm::ConstantInt>(AI->getArraySize()))
                    continue;
                if (!AI->getAllocatedType()->isSized())
                    continue;
                if (!seen.insert(AI).second)
                    continue;

                auto *arraySizeCI = llvm::cast<llvm::ConstantInt>(AI->getArraySize());
                uint64_t elemBytes = DL.getTypeAllocSize(AI->getAllocatedType());
                uint64_t elemCount = arraySizeCI->getZExtValue();
                uint64_t totalBytes = elemBytes * elemCount;
                if (totalBytes > std::numeric_limits<unsigned>::max())
                    continue;

                candidates_.push_back(AI);
                candidateSet_.insert(AI);
                varSizeBytes_[AI] = static_cast<unsigned>(totalBytes);
            }
        }
    }
}

void SchematicStateAnalysis::identifyIneligibleSSAValues() {
    llvm::Module *M = F_.getParent();
    if (!M)
        return;

    const llvm::DataLayout &DL = M->getDataLayout();

    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            if (I.getType()->isVoidTy())
                continue;
            if (llvm::isa<llvm::AllocaInst>(&I))
                continue;
            if (llvm::isa<llvm::PHINode>(&I))
                continue;
            if (!I.getType()->isSized())
                continue;

            bool hasCrossBlockUse = false;
            for (const llvm::User *U : I.users()) {
                if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
                    if (UI->getParent() != &BB) {
                        hasCrossBlockUse = true;
                        break;
                    }
                }
            }
            if (!hasCrossBlockUse)
                continue;

            ineligibleObjs_.push_back(&I);
            ineligibleObjSet_.insert(&I);
            varSizeBytes_[&I] = DL.getTypeAllocSize(I.getType());
        }
    }
}

bool SchematicStateAnalysis::isAllowedDirectCall(const llvm::CallBase &CB) const {
    if (CB.isInlineAsm())
        return false;

    llvm::Function *Callee = CB.getCalledFunction();
    if (!Callee)
        return false;

    if (Callee->isIntrinsic())
        return true;

    llvm::StringRef Name = Callee->getName();
    if (Name == "__loop_tripcount")
        return true;
    if (isWhitelistedHelperName(Name))
        return true;

    if (!Callee->isDeclaration())
        return false;

    if (CB.doesNotAccessMemory() || CB.onlyReadsMemory())
        return true;

    return false;
}

void SchematicStateAnalysis::reportStrictError(const llvm::Instruction &I,
                                               const std::string &reason) {
    std::string instStr;
    llvm::raw_string_ostream rso(instStr);
    I.print(rso);

    const auto *BB = I.getParent();
    std::string blockName = BB ? getBlockName(*BB, F_) : "<unknown-bb>";

    std::string msg = "Error: unresolved memory effect in function '" + F_.getName().str() +
                      "', block '" + blockName + "': " + reason + " | inst=" + rso.str();
    analysisErrors_.push_back(std::move(msg));
}

bool SchematicStateAnalysis::validateInstructionForStrictMode(const llvm::Instruction &I) {
    if (candidateGlobals_.empty())
        return true;

    auto mayTouchCandidate = [&](bool checkRef, bool checkMod) {
        for (llvm::GlobalVariable *GV : candidateGlobals_) {
            auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
            llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
            if ((checkRef && llvm::isRefSet(MRI)) || (checkMod && llvm::isModSet(MRI)))
                return true;
        }
        return false;
    };

    if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
        if (!isAllowedDirectCall(*CB)) {
            reportStrictError(I, "call target/effects are unresolved in strict mode");
            return false;
        }
        return true;
    }

    if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        const llvm::Value *Obj =
            llvm::getUnderlyingObject(LI->getPointerOperand()->stripPointerCasts());
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Obj)) {
            if (candidateSet_.count(const_cast<llvm::GlobalVariable *>(GV)))
                return true;
        }
        if (llvm::isa<llvm::AllocaInst>(Obj))
            return true;
        if (mayTouchCandidate(true, false)) {
            reportStrictError(I, "load may alias candidate globals but target is unresolved");
            return false;
        }
        return true;
    }

    if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        const llvm::Value *Obj =
            llvm::getUnderlyingObject(SI->getPointerOperand()->stripPointerCasts());
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Obj)) {
            if (candidateSet_.count(const_cast<llvm::GlobalVariable *>(GV)))
                return true;
        }
        if (llvm::isa<llvm::AllocaInst>(Obj))
            return true;
        if (mayTouchCandidate(false, true)) {
            reportStrictError(I, "store may alias candidate globals but target is unresolved");
            return false;
        }
        return true;
    }

    if (auto *MI = llvm::dyn_cast<llvm::MemIntrinsic>(&I)) {
        bool directCandidate = false;
        if (auto *MCI = llvm::dyn_cast<llvm::MemCpyInst>(MI)) {
            const llvm::Value *DstObj =
                llvm::getUnderlyingObject(MCI->getDest()->stripPointerCasts());
            const llvm::Value *SrcObj =
                llvm::getUnderlyingObject(MCI->getSource()->stripPointerCasts());
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(DstObj))
                directCandidate |= candidateSet_.count(const_cast<llvm::GlobalVariable *>(GV)) > 0;
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(SrcObj))
                directCandidate |= candidateSet_.count(const_cast<llvm::GlobalVariable *>(GV)) > 0;
            if (llvm::isa<llvm::AllocaInst>(DstObj) || llvm::isa<llvm::AllocaInst>(SrcObj))
                directCandidate = true;
        } else {
            const llvm::Value *DstObj =
                llvm::getUnderlyingObject(MI->getRawDest()->stripPointerCasts());
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(DstObj))
                directCandidate |= candidateSet_.count(const_cast<llvm::GlobalVariable *>(GV)) > 0;
            if (llvm::isa<llvm::AllocaInst>(DstObj))
                directCandidate = true;
        }

        if (!directCandidate && mayTouchCandidate(true, true)) {
            reportStrictError(I,
                              "mem intrinsic may touch candidate globals via unresolved pointer");
            return false;
        }
        return true;
    }

    if (I.mayReadOrWriteMemory() && !llvm::isa<llvm::AllocaInst>(&I) &&
        !llvm::isa<llvm::PHINode>(&I)) {
        if (mayTouchCandidate(true, true)) {
            reportStrictError(I, "memory instruction touching candidate globals is unresolved");
            return false;
        }
    }

    return true;
}

void SchematicStateAnalysis::computeAccessMaps() {
    // For globals: AA-based counting.
    // For allocas: user-scan based counting.

    for (llvm::BasicBlock &BB : F_) {
        const llvm::BasicBlock *BBKey = &BB;

        for (llvm::Instruction &I : BB) {
            validateInstructionForStrictMode(I);

            // Process candidate globals via AA.
            for (llvm::GlobalVariable *GV : candidateGlobals_) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                auto key = std::make_pair(BBKey, static_cast<llvm::Value *>(GV));

                if (llvm::isRefSet(MRI))
                    loadCounts_[key]++;
                if (llvm::isModSet(MRI))
                    storeCounts_[key]++;
            }
        }
    }

    // Process candidate allocas via user scanning.
    for (llvm::Value *V : candidates_) {
        auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V);
        if (!AI)
            continue;

        for (const llvm::User *U : AI->users()) {
            if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(U)) {
                if (llvm::getUnderlyingObject(LI->getPointerOperand()->stripPointerCasts()) == AI) {
                    auto key =
                        std::make_pair(static_cast<const llvm::BasicBlock *>(LI->getParent()),
                                       static_cast<llvm::Value *>(AI));
                    loadCounts_[key]++;
                }
            } else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(U)) {
                if (llvm::getUnderlyingObject(SI->getPointerOperand()->stripPointerCasts()) == AI) {
                    auto key =
                        std::make_pair(static_cast<const llvm::BasicBlock *>(SI->getParent()),
                                       static_cast<llvm::Value *>(AI));
                    storeCounts_[key]++;
                }
            }
        }
    }
}

} // namespace checkpoint
