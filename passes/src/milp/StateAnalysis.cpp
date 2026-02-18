#include "milp/StateAnalysis.h"
#include "milp/LivenessAnalysis.h"

#include "common/AnnotationUtils.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <queue>

namespace checkpoint {

namespace {

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
const std::set<llvm::Value *> StateAnalysis::emptyValueSet_;

StateAnalysis::StateAnalysis(llvm::Function &F,
                             llvm::AAResults &AA,
                             const CFGAnalysis &cfg)
    : F_(F), AA_(AA), cfg_(cfg) {
    buildBlockMap();
    identifyVMObjs();
    identifyIneligibleObjs();
    identifyIneligibleSSAValues();
    computeAccessMaps();
    computeEligLiveness();
    computeIneligGlobalAllocaLiveness();
    computeIneligSSALiveness();
}

bool StateAnalysis::isCandidateGlobal(llvm::GlobalVariable *gv) const {
    return vmObjSet_.count(gv) > 0;
}

const std::vector<llvm::Value *> &
StateAnalysis::getIneligibleObjs() const {
    return ineligibleObjs_;
}

bool StateAnalysis::isIneligible(llvm::Value *v) const {
    return ineligibleObjSet_.count(v) > 0;
}

void StateAnalysis::printAnalysisErrors(llvm::raw_ostream &os) const {
    for (const auto &msg : analysisErrors_) {
        os << msg << "\n";
    }
}

const std::set<llvm::GlobalVariable *> &
StateAnalysis::getEligLiveIn(const std::string &block) const {
    auto it = eligLiveIn_.find(block);
    if (it != eligLiveIn_.end())
        return it->second;
    return emptyGVSet_;
}

bool StateAnalysis::getEligDefIndicator(const std::string &block,
                                        llvm::GlobalVariable *gv) const {
    auto it = eligDefGlobals_.find(block);
    if (it == eligDefGlobals_.end())
        return false;
    return it->second.count(gv) > 0;
}

const std::set<llvm::Value *> &
StateAnalysis::getIneligLiveIn(const std::string &block) const {
    auto it = ineligLiveIn_.find(block);
    if (it != ineligLiveIn_.end())
        return it->second;
    return emptyValueSet_;
}

bool StateAnalysis::getIneligDefIndicator(const std::string &block,
                                          llvm::Value *v) const {
    auto it = ineligDefVars_.find(block);
    if (it == ineligDefVars_.end())
        return false;
    return it->second.count(v) > 0;
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

unsigned StateAnalysis::getVarSizeBytes(llvm::Value *v) const {
    auto it = varSizeBytes_.find(v);
    if (it != varSizeBytes_.end()) {
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
    return checkpoint::isMilpCandidateAnnotated(GV, F_.getParent());
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
        varSizeBytes_[&GV] = sizeBytes;
    }
}

void StateAnalysis::identifyIneligibleObjs() {
    llvm::Module *M = F_.getParent();
    if (!M)
        return;

    const llvm::DataLayout &DL = M->getDataLayout();

    // --- Ineligible globals: non-candidate globals accessed in the function ---
    std::set<llvm::GlobalVariable *> seenGV;
    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            const llvm::Value *Ptr = nullptr;
            if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I))
                Ptr = LI->getPointerOperand();
            else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I))
                Ptr = SI->getPointerOperand();
            else
                continue;

            const llvm::Value *Obj =
                llvm::getUnderlyingObject(Ptr->stripPointerCasts());
            auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(
                const_cast<llvm::Value *>(Obj));
            if (!GV)
                continue;

            // Skip candidates — they're already in vmObjs_.
            if (vmObjSet_.count(GV))
                continue;

            // Structural filters (same as identifyVMObjs).
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

            if (!seenGV.insert(GV).second)
                continue;

            ineligibleObjs_.push_back(GV);
            ineligibleObjSet_.insert(GV);
            unsigned sizeBytes = DL.getTypeAllocSize(GV->getValueType());
            varSizeBytes_[GV] = sizeBytes;
        }
    }

    // --- Ineligible allocas: static stack allocations ---
    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            auto *AI = llvm::dyn_cast<llvm::AllocaInst>(&I);
            if (!AI)
                continue;

            // Dynamic allocas have runtime-dependent sizes, so we cannot create
            // fixed-size NVM backups or account for them in the MILP VM capacity
            // constraint.
            if (!llvm::isa<llvm::ConstantInt>(AI->getArraySize())) {
                llvm::report_fatal_error(
                    "MILP checkpoint pass: dynamic alloca '%" +
                    AI->getName() + "' in function '" + F_.getName() +
                    "' cannot be checkpointed (runtime-dependent size "
                    "prevents fixed-size NVM backup and MILP capacity "
                    "modeling)");
            }

            if (!AI->getAllocatedType()->isSized())
                continue;

            ineligibleObjs_.push_back(AI);
            ineligibleObjSet_.insert(AI);
            unsigned sizeBytes = DL.getTypeAllocSize(AI->getAllocatedType());
            varSizeBytes_[AI] = sizeBytes;
        }
    }
}

void StateAnalysis::identifyIneligibleSSAValues() {
    llvm::Module *M = F_.getParent();
    if (!M)
        return;

    const llvm::DataLayout &DL = M->getDataLayout();

    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            // Skip void, alloca (already handled), and PHI nodes at this stage
            // (PHIs are handled via liveness).
            if (I.getType()->isVoidTy())
                continue;
            if (llvm::isa<llvm::AllocaInst>(&I))
                continue;

            // Unsized types (void, label, token, metadata) cannot be stored to
            // memory and don't represent runtime data state — void produces no
            // value, label/token are compile-time control flow constructs,
            // metadata is stripped during codegen. Skipping them is both
            // necessary (LLVM verifier rejects stores of these types) and
            // correct (they don't need checkpointing).
            if (!I.getType()->isSized())
                continue;

            // Check if any user is in a different block (cross-block use).
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
            unsigned sizeBytes = DL.getTypeAllocSize(I.getType());
            varSizeBytes_[&I] = sizeBytes;
        }
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
        return true;
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
    // Collect ineligible globals for AA-based access tracking.
    std::vector<llvm::GlobalVariable *> ineligGlobals;
    for (llvm::Value *V : ineligibleObjs_) {
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
            ineligGlobals.push_back(GV);
    }

    for (llvm::BasicBlock &BB : F_) {
        std::string blockName = getBlockName(BB, F_);

        for (llvm::Instruction &I : BB) {
            validateInstructionForStrictMode(I);

            // Process candidate globals (eligible).
            auto processEligGV = [&](llvm::GlobalVariable *GV) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                auto key = std::make_pair(blockName, GV);

                if (llvm::isRefSet(MRI))
                    loadCounts_[key]++;
                if (llvm::isModSet(MRI)) {
                    storeCounts_[key]++;
                    eligDefGlobals_[blockName].insert(GV);
                }
            };

            for (llvm::GlobalVariable *GV : vmObjs_)
                processEligGV(GV);

            // Process ineligible globals (for NVM penalty and def tracking).
            auto processIneligGV = [&](llvm::GlobalVariable *GV) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                auto key = std::make_pair(blockName, GV);

                if (llvm::isRefSet(MRI))
                    loadCounts_[key]++;
                if (llvm::isModSet(MRI)) {
                    storeCounts_[key]++;
                    ineligDefVars_[blockName].insert(GV);
                }
            };

            for (llvm::GlobalVariable *GV : ineligGlobals)
                processIneligGV(GV);
        }
    }

    // Alloca def tracking: scan users of each ineligible AllocaInst.
    for (llvm::Value *V : ineligibleObjs_) {
        auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V);
        if (!AI)
            continue;
        for (const llvm::User *U : AI->users()) {
            if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(U)) {
                if (SI->getPointerOperand()->stripPointerCasts() == AI) {
                    std::string blockName = getBlockName(*SI->getParent(), F_);
                    ineligDefVars_[blockName].insert(AI);
                }
            }
        }
    }

    // SSA value def tracking: the defining instruction IS the def.
    for (llvm::Value *V : ineligibleObjs_) {
        auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
        if (!Inst || llvm::isa<llvm::AllocaInst>(Inst))
            continue;
        std::string blockName = getBlockName(*Inst->getParent(), F_);
        ineligDefVars_[blockName].insert(Inst);
    }
}

void StateAnalysis::computeEligLiveness() {
    eligLiveIn_ = checkpoint::computeEligibleLiveness(
        F_, AA_, cfg_, vmObjs_, nameToBlock_);
}

void StateAnalysis::computeIneligGlobalAllocaLiveness() {
    auto gaLive = checkpoint::computeIneligGlobalAllocaLiveness(
        F_, AA_, cfg_, ineligibleObjs_, nameToBlock_);
    for (auto &[block, vals] : gaLive)
        ineligLiveIn_[block].insert(vals.begin(), vals.end());
}

void StateAnalysis::computeIneligSSALiveness() {
    auto ssaLive = checkpoint::computeIneligSSALiveness(
        F_, cfg_, ineligibleObjs_, nameToBlock_);
    for (auto &[block, vals] : ssaLive)
        ineligLiveIn_[block].insert(vals.begin(), vals.end());
}

} // namespace checkpoint
