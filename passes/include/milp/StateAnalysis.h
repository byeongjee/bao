#pragma once

#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Computes candidate-global state data for MILP checkpoint optimization.
///
/// v1 contract:
/// - Candidates are fixed-address globals annotated with "milp_candidate"
/// - Intra-procedural analysis only
/// - Strict unresolved-memory policy (diagnostic + function-level abort)
///
/// V_inelig includes:
/// - Non-candidate globals accessed in the function
/// - Static allocas (stack slots)
/// - Cross-block live SSA instructions (virtual registers)
class StateAnalysis {
public:
    StateAnalysis(llvm::Function &F,
                  llvm::AAResults &AA,
                  const CFGAnalysis &cfg);

    // -- Candidate globals (V_elig) --

    /// All candidate globals considered by MILP.
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const { return vmObjs_; }
    bool isCandidateGlobal(llvm::GlobalVariable *gv) const;

    // -- Eligible live-in/def --

    /// Candidate globals live-in at block b.
    const std::set<llvm::GlobalVariable *> &getEligLiveIn(const std::string &block) const;

    /// D_{b,v}: 1 if eligible v may be defined in block b.
    bool getEligDefIndicator(const std::string &block, llvm::GlobalVariable *gv) const;

    // -- Ineligible objects (V_inelig) --

    /// All ineligible objects: non-candidate globals, allocas, cross-block SSA values.
    const std::vector<llvm::Value *> &getIneligibleObjs() const;
    bool isIneligible(llvm::Value *v) const;

    /// Ineligible objects live-in at block b.
    const std::set<llvm::Value *> &getIneligLiveIn(const std::string &block) const;

    /// D_{b,v}: 1 if ineligible v may be defined in block b.
    bool getIneligDefIndicator(const std::string &block, llvm::Value *v) const;

    // -- Strict-analysis diagnostics --

    bool hasAnalysisErrors() const { return !analysisErrors_.empty(); }
    const std::vector<std::string> &getAnalysisErrors() const {
        return analysisErrors_;
    }
    void printAnalysisErrors(llvm::raw_ostream &os) const;

    // -- Access maps (for energy model) --

    /// Number of loads from global v in block b.
    unsigned getLoadCount(const std::string &block,
                          llvm::GlobalVariable *gv) const;

    /// Number of stores to global v in block b.
    unsigned getStoreCount(const std::string &block,
                           llvm::GlobalVariable *gv) const;

    // -- Mappings --

    /// Get variable size in bytes (works for candidates, ineligible globals,
    /// allocas, and SSA values). Returns 0 for unknown.
    unsigned getVarSizeBytes(llvm::Value *v) const;

    /// Get the LLVM BasicBlock* for a block name.
    llvm::BasicBlock *getBlock(const std::string &name) const;

private:
    llvm::Function &F_;
    llvm::AAResults &AA_;
    const CFGAnalysis &cfg_;

    // Candidate globals (V_elig)
    std::vector<llvm::GlobalVariable *> vmObjs_;
    std::set<llvm::GlobalVariable *> vmObjSet_;
    std::vector<std::string> analysisErrors_;

    // Ineligible objects (V_inelig): non-candidate globals, allocas, SSA values
    std::vector<llvm::Value *> ineligibleObjs_;
    std::set<llvm::Value *> ineligibleObjSet_;

    // Unified size map for all tracked variables (elig + inelig)
    std::map<llvm::Value *, unsigned> varSizeBytes_;

    // Eligible liveness
    std::map<std::string, std::set<llvm::GlobalVariable *>> eligLiveIn_;
    std::map<std::string, std::set<llvm::GlobalVariable *>> eligDefGlobals_;

    // Ineligible liveness
    std::map<std::string, std::set<llvm::Value *>> ineligLiveIn_;
    std::map<std::string, std::set<llvm::Value *>> ineligDefVars_;

    // Access maps: (block, globalVar) -> count (for NVM penalty computation)
    std::map<std::pair<std::string, llvm::GlobalVariable *>, unsigned> loadCounts_;
    std::map<std::pair<std::string, llvm::GlobalVariable *>, unsigned> storeCounts_;

    // Block name -> BasicBlock mapping
    std::map<std::string, llvm::BasicBlock *> nameToBlock_;

    // Empty sets for returning references
    static const std::set<llvm::GlobalVariable *> emptyGVSet_;
    static const std::set<llvm::Value *> emptyValueSet_;

    bool isMilpCandidateAnnotated(llvm::GlobalVariable *GV) const;
    bool isAllowedDirectCall(const llvm::CallBase &CB) const;
    bool validateInstructionForStrictMode(const llvm::Instruction &I);
    void reportStrictError(const llvm::Instruction &I, const std::string &reason);

    void identifyVMObjs();
    void identifyIneligibleObjs();
    void identifyIneligibleSSAValues();
    void buildBlockMap();
    void computeAccessMaps();
    void computeEligLiveness();
    void computeIneligGlobalAllocaLiveness();
    void computeIneligSSALiveness();
};

} // namespace checkpoint
