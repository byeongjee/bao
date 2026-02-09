#pragma once

#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// A definition site: an instruction that defines an SSA register or writes a
/// VMObj (global variable).
struct DefSite {
    unsigned id;
    llvm::Instruction *inst;
    std::string blockName;

    enum Kind { SSAReg, MemoryDef };
    Kind kind;

    llvm::Value *ssaValue;            // non-null for SSAReg
    llvm::GlobalVariable *globalVar;  // non-null for MemoryDef
};

/// A state element that may need checkpoint/restore across region boundaries.
struct StateElement {
    unsigned id;

    enum Kind { Reg, VMObj };
    Kind kind;

    llvm::Value *ssaValue;            // non-null for Reg
    llvm::GlobalVariable *globalVar;  // non-null for VMObj
    unsigned sizeBytes;               // only meaningful for VMObj
};

/// Computes all state-modeling data needed by the MILP (spec Sections 2 + 7).
///
/// Uses LLVM's AAResults for global access identification, DominatorTree for
/// efficient liveness computation, and LoopInfo for loop structure.
class StateAnalysis {
public:
    StateAnalysis(llvm::Function &F,
                  llvm::LoopInfo &LI,
                  llvm::AAResults &AA,
                  llvm::DominatorTree &DT,
                  const CFGAnalysis &cfg);

    // -- VMObj identification --

    /// All global variables considered as VMObjs (excludes constants, intrinsics).
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const { return vmObjs_; }

    // -- State elements --

    /// All state elements (SSA regs + VMObjs).
    const std::vector<StateElement> &getStateElements() const { return stateElements_; }

    /// Get state element by id.
    const StateElement &getStateElement(unsigned id) const { return stateElements_[id]; }

    // -- Definition sites --

    /// All definition sites.
    const std::vector<DefSite> &getDefSites() const { return defSites_; }

    /// Definition sites in a given block.
    const std::vector<unsigned> &getBlockDefSites(const std::string &block) const;

    // -- Liveness --

    /// SSA registers live-in at block b.
    const std::set<llvm::Value *> &getRegLiveIn(const std::string &block) const;

    /// VMObjs live-in at block b.
    const std::set<llvm::GlobalVariable *> &getVMObjLiveIn(const std::string &block) const;

    // -- Reaching definitions --

    /// DefSite IDs for state element s that reach block b.
    /// For SSA regs, this is always a single def (SSA property).
    /// For VMObjs, this is the set of stores that may reach b.
    const std::set<unsigned> &getReachingDefs(const std::string &block,
                                               unsigned stateElemId) const;

    // -- Access maps (for energy model) --

    /// Number of loads from global v in block b.
    unsigned getLoadCount(const std::string &block,
                          llvm::GlobalVariable *gv) const;

    /// Number of stores to global v in block b.
    unsigned getStoreCount(const std::string &block,
                           llvm::GlobalVariable *gv) const;

    // -- Mappings --

    /// Get state element ID for an SSA value (register).
    int getRegStateElemId(llvm::Value *v) const;

    /// Get state element ID for a VMObj global.
    int getVMObjStateElemId(llvm::GlobalVariable *gv) const;

    /// Get the LLVM BasicBlock* for a block name.
    llvm::BasicBlock *getBlock(const std::string &name) const;

private:
    llvm::Function &F_;
    llvm::LoopInfo &LI_;
    llvm::AAResults &AA_;
    llvm::DominatorTree &DT_;
    const CFGAnalysis &cfg_;

    // VMObjs
    std::vector<llvm::GlobalVariable *> vmObjs_;

    // State elements
    std::vector<StateElement> stateElements_;
    std::map<llvm::Value *, unsigned> regToStateElem_;
    std::map<llvm::GlobalVariable *, unsigned> gvToStateElem_;

    // Definition sites
    std::vector<DefSite> defSites_;
    std::map<std::string, std::vector<unsigned>> blockDefSites_;
    // Map: state element id -> set of def site ids
    std::map<unsigned, std::set<unsigned>> stateElemDefSites_;

    // Liveness
    std::map<std::string, std::set<llvm::Value *>> regLiveIn_;
    std::map<std::string, std::set<llvm::GlobalVariable *>> vmObjLiveIn_;

    // Reaching definitions: (block, stateElemId) -> set of DefSite ids
    std::map<std::pair<std::string, unsigned>, std::set<unsigned>> reachingDefs_;

    // Access maps: (block, globalVar) -> count
    std::map<std::pair<std::string, llvm::GlobalVariable *>, unsigned> loadCounts_;
    std::map<std::pair<std::string, llvm::GlobalVariable *>, unsigned> storeCounts_;

    // Block name -> BasicBlock mapping
    std::map<std::string, llvm::BasicBlock *> nameToBlock_;

    // Empty sets for returning references
    static const std::vector<unsigned> emptyDefSiteVec_;
    static const std::set<llvm::Value *> emptyRegSet_;
    static const std::set<llvm::GlobalVariable *> emptyGVSet_;
    static const std::set<unsigned> emptyIdSet_;

    void identifyVMObjs();
    void buildBlockMap();
    void computeDefSites();
    void computeAccessMaps();
    void computeRegLiveness();
    void computeVMObjLiveness();
    void computeReachingDefs();
};

} // namespace checkpoint
