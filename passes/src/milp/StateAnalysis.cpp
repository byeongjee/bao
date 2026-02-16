#include "milp/StateAnalysis.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

namespace checkpoint {

namespace {

static llvm::GlobalVariable *getAnnotatedGlobalFromConst(llvm::Constant *C) {
    if (!C)
        return nullptr;
    C = C->stripPointerCasts();
    return llvm::dyn_cast<llvm::GlobalVariable>(C);
}

static std::string extractAnnotationString(llvm::Constant *AnnoOp) {
    if (!AnnoOp)
        return "";

    llvm::GlobalVariable *StrGV = nullptr;

    if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(AnnoOp)) {
        if (CE->getOpcode() == llvm::Instruction::GetElementPtr) {
            if (auto *Base = llvm::dyn_cast<llvm::Constant>(CE->getOperand(0))) {
                StrGV = llvm::dyn_cast<llvm::GlobalVariable>(Base->stripPointerCasts());
            }
        }
    } else {
        StrGV = llvm::dyn_cast<llvm::GlobalVariable>(AnnoOp->stripPointerCasts());
    }

    if (!StrGV || !StrGV->hasInitializer())
        return "";

    if (auto *CDS = llvm::dyn_cast<llvm::ConstantDataSequential>(StrGV->getInitializer())) {
        if (CDS->isCString())
            return CDS->getAsCString().str();
    }
    return "";
}

static bool isWhitelistedHelperName(llvm::StringRef N) {
    return N.starts_with("__mspabi_") ||
           N.starts_with("__aeabi_") ||
           N.starts_with("__div") ||
           N.starts_with("__udiv") ||
           N.starts_with("__mul") ||
           N.starts_with("__mod");
}

} // namespace

// Static empty containers for safe reference returns
const std::set<llvm::GlobalVariable *> StateAnalysis::emptyGVSet_;

StateAnalysis::StateAnalysis(llvm::Function &F,
                             llvm::AAResults &AA,
                             const CFGAnalysis &cfg)
    : F_(F), AA_(AA), cfg_(cfg) {
    buildBlockMap();
    identifyVMObjs();
    computeAccessMaps();
    computeVMObjLiveness();
}

bool StateAnalysis::isCandidateGlobal(llvm::GlobalVariable *gv) const {
    return vmObjSet_.count(gv) > 0;
}

void StateAnalysis::printAnalysisErrors(llvm::raw_ostream &os) const {
    for (const auto &msg : analysisErrors_) {
        os << msg << "\n";
    }
}

const std::set<llvm::GlobalVariable *> &
StateAnalysis::getVMObjLiveIn(const std::string &block) const {
    auto it = vmObjLiveIn_.find(block);
    if (it != vmObjLiveIn_.end())
        return it->second;
    return emptyGVSet_;
}

const std::set<llvm::GlobalVariable *> &
StateAnalysis::getDefGlobals(const std::string &block) const {
    auto it = defGlobals_.find(block);
    if (it != defGlobals_.end())
        return it->second;
    return emptyGVSet_;
}

bool StateAnalysis::getDefIndicator(const std::string &block,
                                    llvm::GlobalVariable *gv) const {
    auto it = defGlobals_.find(block);
    if (it == defGlobals_.end())
        return false;
    return it->second.count(gv) > 0;
}

unsigned StateAnalysis::getLoadCount(const std::string &block,
                                     llvm::GlobalVariable *gv) const {
    auto key = std::make_pair(block, gv);
    auto it = loadCounts_.find(key);
    if (it != loadCounts_.end())
        return it->second;
    return 0;
}

unsigned StateAnalysis::getStoreCount(const std::string &block,
                                      llvm::GlobalVariable *gv) const {
    auto key = std::make_pair(block, gv);
    auto it = storeCounts_.find(key);
    if (it != storeCounts_.end())
        return it->second;
    return 0;
}

unsigned StateAnalysis::getVMObjSizeBytes(llvm::GlobalVariable *gv) const {
    auto it = vmObjSizeBytes_.find(gv);
    if (it != vmObjSizeBytes_.end()) {
        return it->second;
    }
    return 0;
}

llvm::BasicBlock *StateAnalysis::getBlock(const std::string &name) const {
    auto it = nameToBlock_.find(name);
    if (it != nameToBlock_.end())
        return it->second;
    return nullptr;
}

// ---- Private implementation ----

void StateAnalysis::buildBlockMap() {
    for (llvm::BasicBlock &BB : F_) {
        std::string name = getBlockName(BB, F_);
        nameToBlock_[name] = &BB;
    }
}

bool StateAnalysis::isMilpCandidateAnnotated(llvm::GlobalVariable *GV) const {
    llvm::Module *M = F_.getParent();
    if (!M)
        return false;

    llvm::GlobalVariable *AnnoGV = M->getNamedGlobal("llvm.global.annotations");
    if (!AnnoGV || !AnnoGV->hasInitializer())
        return false;

    auto *CA = llvm::dyn_cast<llvm::ConstantArray>(AnnoGV->getInitializer());
    if (!CA)
        return false;

    for (unsigned i = 0; i < CA->getNumOperands(); ++i) {
        auto *CS = llvm::dyn_cast<llvm::ConstantStruct>(CA->getOperand(i));
        if (!CS || CS->getNumOperands() < 2)
            continue;

        llvm::GlobalVariable *Target =
            getAnnotatedGlobalFromConst(llvm::dyn_cast<llvm::Constant>(CS->getOperand(0)));
        if (Target != GV)
            continue;

        std::string Anno =
            extractAnnotationString(llvm::dyn_cast<llvm::Constant>(CS->getOperand(1)));
        if (Anno == "milp_candidate")
            return true;
    }

    return false;
}

void StateAnalysis::identifyVMObjs() {
    llvm::Module *M = F_.getParent();
    if (!M)
        return;

    const llvm::DataLayout &DL = M->getDataLayout();

    for (llvm::GlobalVariable &GV : M->globals()) {
        // v1: candidate globals are opt-in via annotate("milp_candidate").
        if (!isMilpCandidateAnnotated(&GV))
            continue;

        // Keep only fixed-address, sized globals.
        if (GV.isDeclaration())
            continue;
        if (GV.isConstant())
            continue;
        if (GV.getName().starts_with("llvm."))
            continue;
        if (GV.getName().starts_with("__nvm_"))
            continue;
        if (!GV.getValueType()->isSized())
            continue;

        vmObjs_.push_back(&GV);
        vmObjSet_.insert(&GV);
        unsigned sizeBytes = DL.getTypeAllocSize(GV.getValueType());
        vmObjSizeBytes_[&GV] = sizeBytes;
    }
}

bool StateAnalysis::isAllowedDirectCall(const llvm::CallBase &CB) const {
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

    // v1 contract assumes user-level helper functions are inlined before pass.
    // Non-inlined internal calls are treated as unresolved.
    if (!Callee->isDeclaration())
        return false;

    // Allow external declarations only when memory effects are clearly safe
    // for defs accounting.
    if (CB.doesNotAccessMemory() || CB.onlyReadsMemory())
        return true;

    return false;
}

void StateAnalysis::reportStrictError(const llvm::Instruction &I,
                                      const std::string &reason) {
    std::string instStr;
    llvm::raw_string_ostream rso(instStr);
    I.print(rso);

    const auto *BB = I.getParent();
    std::string blockName = BB ? getBlockName(*BB, F_) : "<unknown-bb>";

    std::string msg =
        "Error: unresolved memory effect in function '" + F_.getName().str() +
        "', block '" + blockName + "': " + reason + " | inst=" + rso.str();
    analysisErrors_.push_back(std::move(msg));
}

bool StateAnalysis::validateInstructionForStrictMode(const llvm::Instruction &I) {
    if (vmObjs_.empty())
        return true;

    auto mayTouchCandidate = [&](bool checkRef, bool checkMod) {
        for (llvm::GlobalVariable *GV : vmObjs_) {
            auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
            llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
            if ((checkRef && llvm::isRefSet(MRI)) ||
                (checkMod && llvm::isModSet(MRI))) {
                return true;
            }
        }
        return false;
    };

    if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
        if (!isAllowedDirectCall(*CB)) {
            reportStrictError(I, "call target/effects are unresolved in strict mode");
            return false;
        }
    }

    if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        const llvm::Value *Obj =
            llvm::getUnderlyingObject(LI->getPointerOperand()->stripPointerCasts());
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Obj)) {
            if (isCandidateGlobal(const_cast<llvm::GlobalVariable *>(GV)))
                return true;
        }
        if (mayTouchCandidate(/*ref*/ true, /*mod*/ false)) {
            reportStrictError(I, "load may alias candidate globals but target is unresolved");
            return false;
        }
        return true;
    }

    if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        const llvm::Value *Obj =
            llvm::getUnderlyingObject(SI->getPointerOperand()->stripPointerCasts());
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Obj)) {
            if (isCandidateGlobal(const_cast<llvm::GlobalVariable *>(GV)))
                return true;
        }
        if (mayTouchCandidate(/*ref*/ false, /*mod*/ true)) {
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
                directCandidate |=
                    isCandidateGlobal(const_cast<llvm::GlobalVariable *>(GV));
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(SrcObj))
                directCandidate |=
                    isCandidateGlobal(const_cast<llvm::GlobalVariable *>(GV));
        } else {
            const llvm::Value *DstObj =
                llvm::getUnderlyingObject(MI->getRawDest()->stripPointerCasts());
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(DstObj))
                directCandidate |=
                    isCandidateGlobal(const_cast<llvm::GlobalVariable *>(GV));
        }

        if (!directCandidate && mayTouchCandidate(/*ref*/ true, /*mod*/ true)) {
            reportStrictError(I, "mem intrinsic may touch candidate globals via unresolved pointer");
            return false;
        }
        return true;
    }

    if (I.mayReadOrWriteMemory() &&
        !llvm::isa<llvm::AllocaInst>(&I) &&
        !llvm::isa<llvm::PHINode>(&I)) {
        if (mayTouchCandidate(/*ref*/ true, /*mod*/ true)) {
            reportStrictError(I, "memory instruction touching candidate globals is unresolved");
            return false;
        }
    }

    return true;
}

void StateAnalysis::computeAccessMaps() {
    for (llvm::BasicBlock &BB : F_) {
        std::string blockName = getBlockName(BB, F_);

        for (llvm::Instruction &I : BB) {
            validateInstructionForStrictMode(I);

            for (llvm::GlobalVariable *GV : vmObjs_) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                auto key = std::make_pair(blockName, GV);

                if (llvm::isRefSet(MRI))
                    loadCounts_[key]++;
                if (llvm::isModSet(MRI)) {
                    storeCounts_[key]++;
                    defGlobals_[blockName].insert(GV);
                }
            }
        }
    }
}

void StateAnalysis::computeVMObjLiveness() {
    if (vmObjs_.empty())
        return;

    struct BlockGVInfo {
        bool loadBeforeMustStore = false;
        bool hasMustStore = false;
    };

    std::map<std::pair<std::string, llvm::GlobalVariable *>, BlockGVInfo> blockGVInfo;

    for (llvm::BasicBlock &BB : F_) {
        std::string blockName = getBlockName(BB, F_);

        for (llvm::GlobalVariable *GV : vmObjs_) {
            auto key = std::make_pair(blockName, GV);
            BlockGVInfo info;
            bool seenMustStore = false;

            for (llvm::Instruction &I : BB) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);

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

    for (llvm::GlobalVariable *GV : vmObjs_) {
        std::map<std::string, bool> liveIn, liveOut;
        for (const auto &blockName : cfg_.getBlocks()) {
            liveIn[blockName] = false;
            liveOut[blockName] = false;
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &blockName : cfg_.getBlocks()) {
                auto key = std::make_pair(blockName, GV);
                const BlockGVInfo &info = blockGVInfo[key];

                bool newLiveOut = false;
                llvm::BasicBlock *BB = nameToBlock_[blockName];
                for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    std::string succName = getBlockName(*Succ, F_);
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

        for (const auto &blockName : cfg_.getBlocks()) {
            if (liveIn[blockName])
                vmObjLiveIn_[blockName].insert(GV);
        }
    }
}

} // namespace checkpoint
