#pragma once

#include "common/CFGAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// State analysis for SCHEMATIC pass.
///
/// Unlike MILP's StateAnalysis, this treats both globals and allocas as
/// placement candidates (eligible for VM/NVM optimization). Only cross-block
/// SSA values are ineligible.
///
/// No liveness analysis is performed — IntervalAllocator computes
/// interval-local liveness via computeLivenessFlags().
class SchematicStateAnalysis {
  public:
    SchematicStateAnalysis(llvm::Function &F, llvm::AAResults &AA, const CFGAnalysis &cfg);

    // -- Placement candidates (globals + allocas) --

    const std::vector<llvm::Value *> &getCandidates() const { return candidates_; }
    bool isCandidate(llvm::Value *v) const { return candidateSet_.count(v) > 0; }

    // -- Access counts --

    unsigned getLoadCount(const llvm::BasicBlock *BB, llvm::Value *v) const;
    unsigned getStoreCount(const llvm::BasicBlock *BB, llvm::Value *v) const;

    // -- First operation type --

    /// Returns true if first operation on v in BB is a load, false if store.
    /// Returns nullopt if v is not accessed in BB.
    std::optional<bool> getFirstOpIsLoad(const llvm::BasicBlock *BB, llvm::Value *v) const;

    // -- Variable sizes --

    unsigned getVarSizeBytes(llvm::Value *v) const;

    // -- Ineligible objects (cross-block SSA values only) --

    const std::vector<llvm::Value *> &getIneligibleObjs() const { return ineligibleObjs_; }
    bool isIneligible(llvm::Value *v) const { return ineligibleObjSet_.count(v) > 0; }

    // -- Strict-analysis diagnostics --

    bool hasAnalysisErrors() const { return !analysisErrors_.empty(); }
    const std::vector<std::string> &getAnalysisErrors() const { return analysisErrors_; }
    void printAnalysisErrors(llvm::raw_ostream &os) const;

  private:
    llvm::Function &F_;
    llvm::AAResults &AA_;

    // Placement candidates: globals + allocas
    std::vector<llvm::Value *> candidates_;
    std::set<llvm::Value *> candidateSet_;

    // Candidate globals subset (for AA-based access counting)
    std::vector<llvm::GlobalVariable *> candidateGlobals_;

    // Ineligible objects: cross-block SSA values only
    std::vector<llvm::Value *> ineligibleObjs_;
    std::set<llvm::Value *> ineligibleObjSet_;

    // Variable sizes
    std::map<llvm::Value *, unsigned> varSizeBytes_;

    // Access maps: (block, value) -> count
    std::map<std::pair<const llvm::BasicBlock *, llvm::Value *>, unsigned> loadCounts_;
    std::map<std::pair<const llvm::BasicBlock *, llvm::Value *>, unsigned> storeCounts_;

    // First operation type: (block, value) -> true if first op is load
    std::map<std::pair<const llvm::BasicBlock *, llvm::Value *>, bool> firstOpIsLoad_;

    std::vector<std::string> analysisErrors_;

    bool isAllowedDirectCall(const llvm::CallBase &CB) const;
    bool validateInstructionForStrictMode(const llvm::Instruction &I);
    void reportStrictError(const llvm::Instruction &I, const std::string &reason);

    void identifyCandidates();
    void identifyIneligibleSSAValues();
    void computeAccessMaps();
};

} // namespace checkpoint
