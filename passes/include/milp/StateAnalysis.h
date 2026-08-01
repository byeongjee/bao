#pragma once

#include "common/CFGAnalysis.h"

#include "llvm/ADT/DenseMap.h"
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

/// Resolve \p Ptr to its unique underlying GlobalVariable, looking through
/// phi/select (unlike llvm::getUnderlyingObject). Returns nullptr when the
/// base is not provably a single global — e.g. a select between two globals,
/// or a base the lookup cannot resolve. Shared by the strict-mode validator
/// and the VM-region access rewrite so both agree on which accesses are
/// statically redirectable to a shadow.
llvm::GlobalVariable *resolveUniqueUnderlyingGlobal(const llvm::Value *Ptr);

/// Computes candidate-global state data for MILP checkpoint optimization.
///
/// All directly-accessed globals in the function are candidates for VM/NVM
/// placement. Intra-procedural analysis only with strict unresolved-memory
/// policy: unresolved effects are diagnostics that the MILP pass turns into
/// a fatal compile error.
///
/// V_inelig includes:
/// - Static allocas (stack slots)
/// - Cross-block live SSA instructions (virtual registers)
class StateAnalysis {
  public:
    StateAnalysis(llvm::Function &F, llvm::AAResults &AA, const CFGAnalysis &cfg);

    // -- Candidate globals (V_elig) --

    /// All candidate globals considered by MILP.
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const { return vmObjs_; }
    bool isCandidateGlobal(llvm::GlobalVariable *gv) const;

    // -- Eligible live-in/def --

    /// Candidate globals live-in at block b.
    const std::set<llvm::GlobalVariable *> &getEligLiveIn(const llvm::BasicBlock *BB) const;

    /// D_{b,v}: 1 if eligible v may be defined in block b.
    bool getEligDefIndicator(const llvm::BasicBlock *BB, llvm::GlobalVariable *gv) const;

    // -- Ineligible objects (V_inelig) --

    /// All ineligible objects: allocas and cross-block SSA values.
    const std::vector<llvm::Value *> &getIneligibleObjs() const;
    bool isIneligible(llvm::Value *v) const;

    /// Ineligible objects live-in at block b.
    const std::set<llvm::Value *> &getIneligLiveIn(const llvm::BasicBlock *BB) const;

    /// D_{b,v}: 1 if ineligible v may be defined in block b.
    bool getIneligDefIndicator(const llvm::BasicBlock *BB, llvm::Value *v) const;

    // -- Strict-analysis diagnostics --

    bool hasAnalysisErrors() const { return !analysisErrors_.empty(); }
    const std::vector<std::string> &getAnalysisErrors() const { return analysisErrors_; }
    void printAnalysisErrors(llvm::raw_ostream &os) const;

    // -- Access maps (for energy model) --

    /// Number of loads from global v in block b.
    unsigned getLoadCount(const llvm::BasicBlock *BB, llvm::GlobalVariable *gv) const;

    /// Number of stores to global v in block b.
    unsigned getStoreCount(const llvm::BasicBlock *BB, llvm::GlobalVariable *gv) const;

    // -- Mappings --

    /// Get variable size in bytes (works for candidate globals, allocas,
    /// and SSA values). Returns 0 for unknown.
    unsigned getVarSizeBytes(llvm::Value *v) const;

  private:
    llvm::Function &F_;
    llvm::AAResults &AA_;
    const CFGAnalysis &cfg_;

    // Candidate globals (V_elig)
    std::vector<llvm::GlobalVariable *> vmObjs_;
    std::set<llvm::GlobalVariable *> vmObjSet_;
    std::vector<std::string> analysisErrors_;

    // Ineligible objects (V_inelig): allocas, cross-block SSA values
    std::vector<llvm::Value *> ineligibleObjs_;
    std::set<llvm::Value *> ineligibleObjSet_;

    // Unified size map for all tracked variables (elig + inelig)
    std::map<llvm::Value *, unsigned> varSizeBytes_;

    // Eligible liveness
    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> eligLiveIn_;
    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> eligDefGlobals_;

    // Ineligible liveness
    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>> ineligLiveIn_;
    llvm::DenseMap<const llvm::BasicBlock *, std::set<llvm::Value *>> ineligDefVars_;

    // Access maps: (block, globalVar) -> count (for NVM penalty computation)
    std::map<std::pair<const llvm::BasicBlock *, llvm::GlobalVariable *>, unsigned> loadCounts_;
    std::map<std::pair<const llvm::BasicBlock *, llvm::GlobalVariable *>, unsigned> storeCounts_;

    // Empty sets for returning references
    static const std::set<llvm::GlobalVariable *> emptyGVSet_;
    static const std::set<llvm::Value *> emptyValueSet_;

    bool isAllowedDirectCall(const llvm::CallBase &CB) const;
    bool validateInstructionForStrictMode(const llvm::Instruction &I);
    void reportStrictError(const llvm::Instruction &I, const std::string &reason);

    void identifyVMObjs();
    void identifyIneligibleObjs();
    void identifyIneligibleSSAValues();
    void computeAccessMaps();
    void computeEligLiveness();
    void computeIneligAllocaLiveness();
    void computeIneligSSALiveness();
};

} // namespace checkpoint
