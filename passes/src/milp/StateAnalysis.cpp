#include "milp/StateAnalysis.h"
#include "milp/LivenessAnalysis.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InstIterator.h"
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

static bool isEligibleGlobal(const llvm::GlobalVariable &GV) {
    return !GV.isDeclaration() && !GV.isConstant() && GV.getValueType()->isSized() &&
           !GV.getName().starts_with("llvm.") && !GV.getName().starts_with("__nvm_") &&
           !GV.getName().starts_with("__vm_shadow_");
}

/// The unique eligible global a load/store directly accesses, or null.
static llvm::GlobalVariable *eligibleAccessTarget(const llvm::Instruction &I) {
    const llvm::Value *Ptr = nullptr;
    if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I))
        Ptr = LI->getPointerOperand();
    else if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I))
        Ptr = SI->getPointerOperand();
    else
        return nullptr;

    // Same resolution the instrumenter uses: it looks through phi/select, so
    // a global reached only through a pointer induction variable is still
    // found.
    llvm::GlobalVariable *GV = resolveUniqueUnderlyingGlobal(Ptr);
    return GV && isEligibleGlobal(*GV) ? GV : nullptr;
}

/// Whether I may read or write GV through a pointer that does not resolve to
/// a unique global.
static bool mayAccessThroughUnresolvedPointer(llvm::AAResults &AA, const llvm::Instruction &I,
                                              llvm::GlobalVariable &GV) {
    llvm::ModRefInfo MRI = AA.getModRefInfo(&I, llvm::MemoryLocation::getBeforeOrAfter(&GV));
    if (MRI == llvm::ModRefInfo::NoModRef)
        return false;

    auto unresolved = [](const llvm::Value *Ptr) { return !resolveUniqueUnderlyingGlobal(Ptr); };

    if (auto *LI = llvm::dyn_cast<llvm::LoadInst>(&I))
        return unresolved(LI->getPointerOperand()) && llvm::isRefSet(MRI);
    if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I))
        return unresolved(SI->getPointerOperand()) && llvm::isModSet(MRI);
    if (auto *MT = llvm::dyn_cast<llvm::MemTransferInst>(&I))
        return (unresolved(MT->getRawDest()) && llvm::isModSet(MRI)) ||
               (unresolved(MT->getSource()) && llvm::isRefSet(MRI));
    if (auto *MI = llvm::dyn_cast<llvm::MemIntrinsic>(&I))
        return unresolved(MI->getRawDest()) && llvm::isModSet(MRI);
    if (llvm::isa<llvm::CallBase>(I) || llvm::isa<llvm::AllocaInst>(I) ||
        llvm::isa<llvm::PHINode>(I))
        return false;
    return I.mayReadOrWriteMemory();
}

} // namespace

llvm::GlobalVariable *resolveUniqueUnderlyingGlobal(const llvm::Value *Ptr) {
    llvm::SmallVector<const llvm::Value *, 4> objs;
    llvm::getUnderlyingObjects(Ptr, objs);
    if (objs.size() != 1)
        return nullptr;
    return const_cast<llvm::GlobalVariable *>(llvm::dyn_cast<llvm::GlobalVariable>(objs.front()));
}

// Static empty containers for safe reference returns
const std::set<llvm::GlobalVariable *> StateAnalysis::emptyGVSet_;
const std::set<llvm::Value *> StateAnalysis::emptyValueSet_;

StateAnalysis::StateAnalysis(llvm::Function &F, llvm::AAResults &AA, const CFGAnalysis &cfg)
    : F_(F), AA_(AA), cfg_(cfg) {
    identifyVMObjs();
    validateCalls();
    identifyIneligibleObjs();
    identifyIneligibleSSAValues();
    computeAccessMaps();
    computeEligLiveness();
    computeIneligAllocaLiveness();
    computeIneligSSALiveness();
}

bool StateAnalysis::isCandidateGlobal(llvm::GlobalVariable *gv) const {
    return vmObjSet_.count(gv) > 0;
}

const std::vector<llvm::Value *> &StateAnalysis::getIneligibleObjs() const {
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
StateAnalysis::getEligLiveIn(const llvm::BasicBlock *BB) const {
    auto it = eligLiveIn_.find(BB);
    if (it != eligLiveIn_.end())
        return it->second;
    return emptyGVSet_;
}

bool StateAnalysis::getEligDefIndicator(const llvm::BasicBlock *BB,
                                        llvm::GlobalVariable *gv) const {
    auto it = eligDefGlobals_.find(BB);
    if (it == eligDefGlobals_.end())
        return false;
    return it->second.count(gv) > 0;
}

const std::set<llvm::Value *> &StateAnalysis::getIneligLiveIn(const llvm::BasicBlock *BB) const {
    auto it = ineligLiveIn_.find(BB);
    if (it != ineligLiveIn_.end())
        return it->second;
    return emptyValueSet_;
}

bool StateAnalysis::getIneligDefIndicator(const llvm::BasicBlock *BB, llvm::Value *v) const {
    auto it = ineligDefVars_.find(BB);
    if (it == ineligDefVars_.end())
        return false;
    return it->second.count(v) > 0;
}

unsigned StateAnalysis::getLoadCount(const llvm::BasicBlock *BB, llvm::GlobalVariable *gv) const {
    auto key = std::make_pair(BB, gv);
    auto it = loadCounts_.find(key);
    if (it != loadCounts_.end())
        return it->second;
    return 0;
}

unsigned StateAnalysis::getStoreCount(const llvm::BasicBlock *BB, llvm::GlobalVariable *gv) const {
    auto key = std::make_pair(BB, gv);
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

// ---- Private implementation ----

// The candidate set is the directly-accessed globals minus those an
// unresolved access may alias: such a global cannot be redirected to a VM
// shadow consistently, so it is conservatively kept in NVM.
void StateAnalysis::identifyVMObjs() {
    std::vector<llvm::GlobalVariable *> accessed = directlyAccessedGlobals();
    std::set<llvm::GlobalVariable *> excluded = globalsAliasedByUnresolvedAccesses(accessed);

    for (llvm::GlobalVariable *GV : excluded)
        PLOGW << "StateAnalysis: global '" << GV->getName()
              << "' may be aliased by an access with an unresolved target; keeping it in NVM";
    nvmKeptGlobals_.assign(excluded.begin(), excluded.end());

    auto kept = llvm::make_filter_range(
        accessed, [&](llvm::GlobalVariable *GV) { return !excluded.count(GV); });
    vmObjs_.assign(kept.begin(), kept.end());
    vmObjSet_.insert(vmObjs_.begin(), vmObjs_.end());

    const llvm::DataLayout &DL = F_.getParent()->getDataLayout();
    for (llvm::GlobalVariable *GV : vmObjs_)
        varSizeBytes_[GV] = DL.getTypeAllocSize(GV->getValueType());
}

std::vector<llvm::GlobalVariable *> StateAnalysis::directlyAccessedGlobals() const {
    llvm::SetVector<llvm::GlobalVariable *> globals;
    for (const llvm::Instruction &I : llvm::instructions(F_))
        if (llvm::GlobalVariable *GV = eligibleAccessTarget(I))
            globals.insert(GV);
    return std::vector<llvm::GlobalVariable *>(globals.begin(), globals.end());
}

std::set<llvm::GlobalVariable *> StateAnalysis::globalsAliasedByUnresolvedAccesses(
    const std::vector<llvm::GlobalVariable *> &globals) const {
    auto touched = [&](llvm::GlobalVariable *GV) {
        return llvm::any_of(llvm::instructions(F_), [&](const llvm::Instruction &I) {
            return mayAccessThroughUnresolvedPointer(AA_, I, *GV);
        });
    };
    auto aliased = llvm::make_filter_range(globals, touched);
    return {aliased.begin(), aliased.end()};
}

void StateAnalysis::identifyIneligibleObjs() {
    llvm::Module *M = F_.getParent();
    if (!M)
        return;

    const llvm::DataLayout &DL = M->getDataLayout();

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
                llvm::report_fatal_error("MILP checkpoint pass: dynamic alloca '%" + AI->getName() +
                                         "' in function '" + F_.getName() +
                                         "' cannot be checkpointed (runtime-dependent size "
                                         "prevents fixed-size NVM backup and MILP capacity "
                                         "modeling)");
            }

            if (!AI->getAllocatedType()->isSized())
                continue;

            auto *arraySizeCI = llvm::cast<llvm::ConstantInt>(AI->getArraySize());
            uint64_t elemBytes = DL.getTypeAllocSize(AI->getAllocatedType());
            uint64_t elemCount = arraySizeCI->getZExtValue();
            uint64_t totalBytes = elemBytes * elemCount;
            if (totalBytes > std::numeric_limits<unsigned>::max()) {
                llvm::report_fatal_error("MILP checkpoint pass: alloca '%" + AI->getName() +
                                         "' in function '" + F_.getName() +
                                         "' exceeds size model range");
            }

            ineligibleObjs_.push_back(AI);
            ineligibleObjSet_.insert(AI);
            varSizeBytes_[AI] = static_cast<unsigned>(totalBytes);
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
            // Skip void and alloca (already handled).
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

            // Check if any user is in a different block (cross-block use),
            // or if this is a PHI with same-block users.  PHI values are
            // defined before the boundary insertion point; when a region
            // boundary splits the block, same-block users end up in the
            // split-off block, making the use effectively cross-block.
            bool needsTracking = false;
            if (llvm::isa<llvm::PHINode>(&I)) {
                needsTracking = !I.use_empty();
            } else {
                for (const llvm::User *U : I.users()) {
                    if (auto *UI = llvm::dyn_cast<llvm::Instruction>(U)) {
                        if (llvm::isa<llvm::PHINode>(UI) || UI->getParent() != &BB) {
                            needsTracking = true;
                            break;
                        }
                    }
                }
            }

            if (!needsTracking)
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
    if (Name == "debug_init" || Name == "debug_exit" || Name == "bench_halt" ||
        Name == "bench_commit_result")
        return true;
    if (Name.starts_with("timing_gpio_"))
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

void StateAnalysis::validateCalls() {
    if (vmObjs_.empty())
        return;

    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
            if (!CB || isAllowedDirectCall(*CB))
                continue;

            std::string instStr;
            llvm::raw_string_ostream rso(instStr);
            I.print(rso);
            analysisErrors_.push_back("Error: unresolved call in function '" + F_.getName().str() +
                                      "', block '" + getBlockName(BB, F_) +
                                      "': target/effects cannot be analyzed | inst=" + rso.str());
        }
    }
}

void StateAnalysis::computeAccessMaps() {
    for (llvm::BasicBlock &BB : F_) {
        const llvm::BasicBlock *BBKey = &BB;

        for (llvm::Instruction &I : BB) {
            // Process candidate globals (eligible).
            auto processEligGV = [&](llvm::GlobalVariable *GV) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                auto key = std::make_pair(BBKey, GV);

                if (llvm::isRefSet(MRI))
                    loadCounts_[key]++;
                if (llvm::isModSet(MRI)) {
                    storeCounts_[key]++;
                    eligDefGlobals_[BBKey].insert(GV);
                }
            };

            for (llvm::GlobalVariable *GV : vmObjs_)
                processEligGV(GV);
        }
    }

    // Alloca def tracking: use alias analysis so writes through GEPs
    // (array elements, struct fields) are visible, matching the
    // eligible-global approach above.
    for (llvm::Value *V : ineligibleObjs_) {
        auto *AI = llvm::dyn_cast<llvm::AllocaInst>(V);
        if (!AI)
            continue;
        auto Loc = llvm::MemoryLocation::getBeforeOrAfter(AI);
        for (llvm::BasicBlock &BB : F_) {
            for (llvm::Instruction &I : BB) {
                if (llvm::isModSet(AA_.getModRefInfo(&I, Loc))) {
                    ineligDefVars_[&BB].insert(AI);
                    break;
                }
            }
        }
    }

    // SSA value def tracking: the defining instruction IS the def.
    for (llvm::Value *V : ineligibleObjs_) {
        auto *Inst = llvm::dyn_cast<llvm::Instruction>(V);
        if (!Inst || llvm::isa<llvm::AllocaInst>(Inst))
            continue;
        const llvm::BasicBlock *BBKey = Inst->getParent();
        ineligDefVars_[BBKey].insert(Inst);
    }
}

void StateAnalysis::computeEligLiveness() {
    eligLiveIn_ = checkpoint::computeEligibleLiveness(F_, AA_, cfg_, vmObjs_);
}

void StateAnalysis::computeIneligAllocaLiveness() {
    auto allocaLive = checkpoint::computeIneligAllocaLiveness(F_, AA_, cfg_, ineligibleObjs_);
    for (auto &[BB, vals] : allocaLive)
        ineligLiveIn_[BB].insert(vals.begin(), vals.end());
}

void StateAnalysis::computeIneligSSALiveness() {
    auto ssaLive = checkpoint::computeIneligSSALiveness(F_, cfg_, ineligibleObjs_);
    for (auto &[BB, vals] : ssaLive)
        ineligLiveIn_[BB].insert(vals.begin(), vals.end());
}

} // namespace checkpoint
