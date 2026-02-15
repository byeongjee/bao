#pragma once

#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Legacy def-site struct kept for compatibility with older call sites.
struct DefSite {
    unsigned id;
    llvm::Instruction *inst;
    std::string blockName;

    enum Kind { SSAReg, MemoryDef };
    Kind kind;

    llvm::Value *ssaValue;            // non-null for SSAReg
    llvm::GlobalVariable *globalVar;  // non-null for MemoryDef
};

/// Legacy state-element struct kept for compatibility with older call sites.
struct StateElement {
    unsigned id;

    enum Kind { Reg, VMObj };
    Kind kind;

    llvm::Value *ssaValue;            // non-null for Reg
    llvm::GlobalVariable *globalVar;  // non-null for VMObj
    unsigned sizeBytes;               // only meaningful for VMObj
};

/// Computes candidate-global state data for MILP checkpoint optimization.
///
/// v1 contract:
/// - Candidates are fixed-address globals annotated with "milp_candidate"
/// - Intra-procedural analysis only
/// - Strict unresolved-memory policy (diagnostic + function-level abort)
class StateAnalysis {
public:
    StateAnalysis(llvm::Function &F,
                  llvm::LoopInfo &LI,
                  llvm::AAResults &AA,
                  llvm::DominatorTree &DT,
                  const CFGAnalysis &cfg);

    // -- Candidate globals --

    /// All candidate globals considered by MILP.
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const { return vmObjs_; }
    bool isCandidateGlobal(llvm::GlobalVariable *gv) const;

    // -- Strict-analysis diagnostics --

    bool hasAnalysisErrors() const { return !analysisErrors_.empty(); }
    const std::vector<std::string> &getAnalysisErrors() const {
        return analysisErrors_;
    }
    void printAnalysisErrors(llvm::raw_ostream &os) const;

    // -- Block-level global data --

    /// Candidate globals live-in at block b.
    const std::set<llvm::GlobalVariable *> &getVMObjLiveIn(const std::string &block) const;

    /// Candidate globals that may be defined in block b.
    const std::set<llvm::GlobalVariable *> &getDefGlobals(const std::string &block) const;

    /// D_{b,v}: 1 if v may be defined in block b.
    bool getDefIndicator(const std::string &block, llvm::GlobalVariable *gv) const;

    // -- Legacy API (compatibility) --

    /// All state elements (SSA regs + VMObjs).
    const std::vector<StateElement> &getStateElements() const { return stateElements_; }

    /// Get state element by id.
    const StateElement &getStateElement(unsigned id) const { return stateElements_[id]; }

    // -- Definition sites --

    /// All definition sites.
    const std::vector<DefSite> &getDefSites() const { return defSites_; }

    /// Definition sites in a given block.
    const std::vector<unsigned> &getBlockDefSites(const std::string &block) const;

    /// Legacy: no register state is modeled in MILP v1.
    const std::set<llvm::Value *> &getRegLiveIn(const std::string &block) const;

    /// Legacy: reaching-def sets are not used in v1 model.
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

    // Candidate globals
    std::vector<llvm::GlobalVariable *> vmObjs_;
    std::set<llvm::GlobalVariable *> vmObjSet_;
    std::vector<std::string> analysisErrors_;

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
    std::map<std::string, std::set<llvm::GlobalVariable *>> defGlobals_;

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

    bool isMilpCandidateAnnotated(llvm::GlobalVariable *GV) const;
    bool isAllowedDirectCall(const llvm::CallBase &CB) const;
    bool validateInstructionForStrictMode(const llvm::Instruction &I);
    void reportStrictError(const llvm::Instruction &I, const std::string &reason);

    void identifyVMObjs();
    void buildBlockMap();
    void computeDefSites();
    void computeAccessMaps();
    void computeRegLiveness();
    void computeVMObjLiveness();
    void computeReachingDefs();
};

} // namespace checkpoint
