#include "milp/StateAnalysis.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace checkpoint {

// Static empty containers for safe reference returns
const std::vector<unsigned> StateAnalysis::emptyDefSiteVec_;
const std::set<llvm::Value *> StateAnalysis::emptyRegSet_;
const std::set<llvm::GlobalVariable *> StateAnalysis::emptyGVSet_;
const std::set<unsigned> StateAnalysis::emptyIdSet_;

StateAnalysis::StateAnalysis(llvm::Function &F,
                             llvm::LoopInfo &LI,
                             llvm::AAResults &AA,
                             llvm::DominatorTree &DT,
                             const CFGAnalysis &cfg)
    : F_(F), LI_(LI), AA_(AA), DT_(DT), cfg_(cfg) {
    buildBlockMap();
    identifyVMObjs();
    computeAccessMaps();
    computeDefSites();
    computeRegLiveness();
    computeVMObjLiveness();
    computeReachingDefs();
}

const std::vector<unsigned> &
StateAnalysis::getBlockDefSites(const std::string &block) const {
    auto it = blockDefSites_.find(block);
    if (it != blockDefSites_.end())
        return it->second;
    return emptyDefSiteVec_;
}

const std::set<llvm::Value *> &
StateAnalysis::getRegLiveIn(const std::string &block) const {
    auto it = regLiveIn_.find(block);
    if (it != regLiveIn_.end())
        return it->second;
    return emptyRegSet_;
}

const std::set<llvm::GlobalVariable *> &
StateAnalysis::getVMObjLiveIn(const std::string &block) const {
    auto it = vmObjLiveIn_.find(block);
    if (it != vmObjLiveIn_.end())
        return it->second;
    return emptyGVSet_;
}

const std::set<unsigned> &
StateAnalysis::getReachingDefs(const std::string &block,
                                unsigned stateElemId) const {
    auto key = std::make_pair(block, stateElemId);
    auto it = reachingDefs_.find(key);
    if (it != reachingDefs_.end())
        return it->second;
    return emptyIdSet_;
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

int StateAnalysis::getRegStateElemId(llvm::Value *v) const {
    auto it = regToStateElem_.find(v);
    if (it != regToStateElem_.end())
        return static_cast<int>(it->second);
    return -1;
}

int StateAnalysis::getVMObjStateElemId(llvm::GlobalVariable *gv) const {
    auto it = gvToStateElem_.find(gv);
    if (it != gvToStateElem_.end())
        return static_cast<int>(it->second);
    return -1;
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

void StateAnalysis::identifyVMObjs() {
    llvm::Module *M = F_.getParent();
    const llvm::DataLayout &DL = M->getDataLayout();

    for (llvm::GlobalVariable &GV : M->globals()) {
        // Skip constants
        if (GV.isConstant())
            continue;

        // Skip LLVM internals (llvm.*)
        if (GV.getName().starts_with("llvm."))
            continue;

        // Skip existing NVM globals (__nvm_*)
        if (GV.getName().starts_with("__nvm_"))
            continue;

        // Skip globals without a proper type
        if (!GV.getValueType()->isSized())
            continue;

        vmObjs_.push_back(&GV);

        // Create state element for this VMObj
        unsigned elemId = stateElements_.size();
        unsigned sizeBytes = DL.getTypeAllocSize(GV.getValueType());
        stateElements_.push_back(
            StateElement{elemId, StateElement::VMObj, nullptr, &GV, sizeBytes});
        gvToStateElem_[&GV] = elemId;
    }
}

void StateAnalysis::computeAccessMaps() {
    for (llvm::BasicBlock &BB : F_) {
        std::string blockName = getBlockName(BB, F_);
        for (llvm::Instruction &I : BB) {
            for (llvm::GlobalVariable *GV : vmObjs_) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                auto key = std::make_pair(blockName, GV);
                if (llvm::isRefSet(MRI))
                    loadCounts_[key]++;
                if (llvm::isModSet(MRI))
                    storeCounts_[key]++;
            }
        }
    }
}

void StateAnalysis::computeDefSites() {
    unsigned nextDefId = 0;

    for (llvm::BasicBlock &BB : F_) {
        std::string blockName = getBlockName(BB, F_);

        for (llvm::Instruction &I : BB) {
            // SSA register defs: non-void, non-PHI, non-alloca instructions
            if (!I.getType()->isVoidTy() &&
                !llvm::isa<llvm::PHINode>(&I) &&
                !llvm::isa<llvm::AllocaInst>(&I)) {

                // Create state element for this SSA reg if not yet created
                if (regToStateElem_.find(&I) == regToStateElem_.end()) {
                    unsigned elemId = stateElements_.size();
                    stateElements_.push_back(
                        StateElement{elemId, StateElement::Reg, &I, nullptr, 0});
                    regToStateElem_[&I] = elemId;
                }

                unsigned elemId = regToStateElem_[&I];
                defSites_.push_back(
                    DefSite{nextDefId, &I, blockName, DefSite::SSAReg, &I, nullptr});
                blockDefSites_[blockName].push_back(nextDefId);
                stateElemDefSites_[elemId].insert(nextDefId);
                nextDefId++;
            }

            // Memory defs: instructions that may write VMObj globals
            for (llvm::GlobalVariable *GV : vmObjs_) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                if (llvm::isModSet(MRI)) {
                    unsigned elemId = gvToStateElem_[GV];
                    defSites_.push_back(DefSite{nextDefId, &I, blockName,
                                                DefSite::MemoryDef, nullptr, GV});
                    blockDefSites_[blockName].push_back(nextDefId);
                    stateElemDefSites_[elemId].insert(nextDefId);
                    nextDefId++;
                }
            }
        }
    }
}

void StateAnalysis::computeRegLiveness() {
    // Standard iterative backward dataflow for SSA register liveness.
    // GEN(b) = SSA values used in b but defined outside b
    // KILL(b) = SSA values defined in b
    // LiveIn(b) = GEN(b) U (LiveOut(b) \ KILL(b))
    // LiveOut(b) = U LiveIn(s) for successors s

    // Compute GEN and KILL for each block
    std::map<std::string, std::set<llvm::Value *>> gen, kill;

    for (llvm::BasicBlock &BB : F_) {
        std::string blockName = getBlockName(BB, F_);

        for (llvm::Instruction &I : BB) {
            // KILL: values defined in this block (if they are tracked as state
            // elements)
            if (regToStateElem_.find(&I) != regToStateElem_.end()) {
                kill[blockName].insert(&I);
            }

            // GEN: operands used in this block that are defined elsewhere
            for (llvm::Use &U : I.operands()) {
                llvm::Value *V = U.get();
                // Only track SSA values that are state elements
                if (regToStateElem_.find(V) == regToStateElem_.end())
                    continue;
                // If defined outside this block, it's a GEN
                if (auto *DefInst = llvm::dyn_cast<llvm::Instruction>(V)) {
                    if (DefInst->getParent() != &BB) {
                        gen[blockName].insert(V);
                    }
                }
            }
        }
    }

    // Iterative fixpoint using reverse postorder (from DominatorTree)
    std::map<std::string, std::set<llvm::Value *>> liveOut;
    bool changed = true;

    while (changed) {
        changed = false;
        // Process in reverse postorder for efficiency
        for (auto *Node : llvm::post_order(DT_.getRootNode())) {
            llvm::BasicBlock *BB = Node->getBlock();
            std::string blockName = getBlockName(*BB, F_);

            // LiveOut(b) = U LiveIn(s) for successors s
            std::set<llvm::Value *> newLiveOut;
            for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                std::string succName = getBlockName(*Succ, F_);
                auto it = regLiveIn_.find(succName);
                if (it != regLiveIn_.end()) {
                    newLiveOut.insert(it->second.begin(), it->second.end());
                }
            }

            // LiveIn(b) = GEN(b) U (LiveOut(b) \ KILL(b))
            std::set<llvm::Value *> newLiveIn = gen[blockName];
            for (llvm::Value *V : newLiveOut) {
                if (kill[blockName].find(V) == kill[blockName].end()) {
                    newLiveIn.insert(V);
                }
            }

            if (newLiveIn != regLiveIn_[blockName]) {
                regLiveIn_[blockName] = std::move(newLiveIn);
                liveOut[blockName] = std::move(newLiveOut);
                changed = true;
            }
        }
    }
}

void StateAnalysis::computeVMObjLiveness() {
    // Per-global backward dataflow using AA access maps.
    // v in LiveIn(b) iff:
    //   - v is loaded in b before any must-store, OR
    //   - v is live-out of b and not must-stored in b
    // Must-store: a block must-stores v if it contains an instruction where
    // AA reports MustMod for v.

    if (vmObjs_.empty())
        return;

    // Precompute per-block, per-global: hasLoad, hasMustStore
    struct BlockGVInfo {
        bool hasLoad = false;
        bool hasMustStore = false;
        bool loadBeforeMustStore = false;
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

                if (llvm::isRefSet(MRI)) {
                    info.hasLoad = true;
                    if (!seenMustStore) {
                        info.loadBeforeMustStore = true;
                    }
                }

                if (llvm::isModSet(MRI)) {
                    // Check if this is a must-store (kills the previous value)
                    // Use isMustSet which checks MustMod
                    if (llvm::isModSet(MRI) &&
                        !llvm::isRefSet(MRI)) {
                        // Conservative: treat as must-store only if it's a
                        // simple store directly to the global (not through
                        // alias)
                        if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                            llvm::Value *Ptr = SI->getPointerOperand()
                                                   ->stripPointerCasts();
                            if (Ptr == GV) {
                                info.hasMustStore = true;
                                seenMustStore = true;
                            }
                        }
                    }
                }
            }
            blockGVInfo[key] = info;
        }
    }

    // Iterative fixpoint per global
    for (llvm::GlobalVariable *GV : vmObjs_) {
        std::map<std::string, bool> liveIn, liveOut;

        // Initialize
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

                // LiveOut(b) = OR LiveIn(s) for successors s
                bool newLiveOut = false;
                llvm::BasicBlock *BB = nameToBlock_[blockName];
                for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                    std::string succName = getBlockName(*Succ, F_);
                    if (liveIn[succName]) {
                        newLiveOut = true;
                        break;
                    }
                }

                // LiveIn(b) = loadBeforeMustStore OR
                //              (liveOut AND NOT hasMustStore)
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

        // Store results
        for (const auto &blockName : cfg_.getBlocks()) {
            if (liveIn[blockName]) {
                vmObjLiveIn_[blockName].insert(GV);
            }
        }
    }
}

void StateAnalysis::computeReachingDefs() {
    // SSA regs: trivial - each value has exactly one def site (SSA property).
    // For each block b and each reg r in LiveIn(b), DefSites(b, r) = {def(r)}.
    for (const auto &blockName : cfg_.getBlocks()) {
        // SSA regs
        auto regIt = regLiveIn_.find(blockName);
        if (regIt != regLiveIn_.end()) {
            for (llvm::Value *V : regIt->second) {
                auto elemIt = regToStateElem_.find(V);
                if (elemIt == regToStateElem_.end())
                    continue;
                unsigned elemId = elemIt->second;
                // Find the single def site for this SSA value
                auto defIt = stateElemDefSites_.find(elemId);
                if (defIt != stateElemDefSites_.end()) {
                    auto key = std::make_pair(blockName, elemId);
                    reachingDefs_[key] = defIt->second;
                }
            }
        }
    }

    // VMObj reaching definitions: per-global forward dataflow.
    // Gen(b) = def sites of v in b (stores to v)
    // Kill(b) = if b has a must-store to v, kill all prior reaching defs
    // ReachIn(b) = U ReachOut(p) for predecessors p
    // ReachOut(b) = Gen(b) U (ReachIn(b) \ Kill(b))  [if must-store]
    //            = Gen(b) U ReachIn(b)                [otherwise]
    for (llvm::GlobalVariable *GV : vmObjs_) {
        auto elemIt = gvToStateElem_.find(GV);
        if (elemIt == gvToStateElem_.end())
            continue;
        unsigned elemId = elemIt->second;

        // Build per-block gen and must-kill info
        std::map<std::string, std::set<unsigned>> genSets;
        std::map<std::string, bool> mustKills;

        for (const auto &blockName : cfg_.getBlocks()) {
            mustKills[blockName] = false;

            // Check for must-store (same logic as liveness)
            llvm::BasicBlock *BB = nameToBlock_[blockName];
            for (llvm::Instruction &I : *BB) {
                auto Loc = llvm::MemoryLocation::getBeforeOrAfter(GV);
                llvm::ModRefInfo MRI = AA_.getModRefInfo(&I, Loc);
                if (llvm::isModSet(MRI)) {
                    if (auto *SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                        llvm::Value *Ptr =
                            SI->getPointerOperand()->stripPointerCasts();
                        if (Ptr == GV) {
                            mustKills[blockName] = true;
                        }
                    }
                }
            }

            // Gen: def sites for this GV in this block
            auto defIt = stateElemDefSites_.find(elemId);
            if (defIt != stateElemDefSites_.end()) {
                for (unsigned dsId : defIt->second) {
                    if (defSites_[dsId].blockName == blockName) {
                        genSets[blockName].insert(dsId);
                    }
                }
            }
        }

        // Fixpoint
        std::map<std::string, std::set<unsigned>> reachIn, reachOut;
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &blockName : cfg_.getBlocks()) {
                // ReachIn(b) = U ReachOut(p) for predecessors p
                std::set<unsigned> newReachIn;
                llvm::BasicBlock *BB = nameToBlock_[blockName];
                for (llvm::BasicBlock *Pred : llvm::predecessors(BB)) {
                    std::string predName = getBlockName(*Pred, F_);
                    auto it = reachOut.find(predName);
                    if (it != reachOut.end()) {
                        newReachIn.insert(it->second.begin(),
                                          it->second.end());
                    }
                }

                // ReachOut(b) = Gen(b) U (must-kill ? {} : ReachIn(b))
                std::set<unsigned> newReachOut = genSets[blockName];
                if (!mustKills[blockName]) {
                    newReachOut.insert(newReachIn.begin(), newReachIn.end());
                }

                if (newReachIn != reachIn[blockName] ||
                    newReachOut != reachOut[blockName]) {
                    reachIn[blockName] = std::move(newReachIn);
                    reachOut[blockName] = std::move(newReachOut);
                    changed = true;
                }
            }
        }

        // Store reaching defs for blocks where this VMObj is live-in
        for (const auto &blockName : cfg_.getBlocks()) {
            auto liveIt = vmObjLiveIn_.find(blockName);
            if (liveIt != vmObjLiveIn_.end() &&
                liveIt->second.count(GV)) {
                auto key = std::make_pair(blockName, elemId);
                reachingDefs_[key] = reachIn[blockName];
            }
        }
    }
}

} // namespace checkpoint
