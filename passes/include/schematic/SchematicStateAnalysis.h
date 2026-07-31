#pragma once

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace checkpoint {

/// State analysis for SCHEMATIC pass.
///
/// Unlike MILP's StateAnalysis, this treats both globals and allocas as
/// placement candidates (eligible for VM/NVM optimization).
///
/// No liveness analysis is performed — MemoryAllocator computes
/// interval-local save/restore needs via computeSaveRestoreFlags().
class SchematicStateAnalysis {
  public:
    SchematicStateAnalysis(llvm::Function &F, llvm::AAResults &AA);

    // -- Placement candidates (globals + allocas) --

    const std::vector<llvm::Value *> &getCandidates() const { return candidates_; }

    // -- Access counts --

    unsigned getLoadCount(const llvm::BasicBlock *BB, llvm::Value *v) const;
    unsigned getStoreCount(const llvm::BasicBlock *BB, llvm::Value *v) const;

    // -- First operation type --

    /// Returns true if first operation on v in BB is a load, false if store.
    /// Returns nullopt if v is not accessed in BB.
    std::optional<bool> getFirstOpIsLoad(const llvm::BasicBlock *BB, llvm::Value *v) const;

    // -- Variable sizes --

    unsigned getVarSizeBytes(llvm::Value *v) const;

    // -- Strict-analysis diagnostics --

    bool hasAnalysisErrors() const { return !analysisErrors_.empty(); }
    const std::vector<std::string> &getAnalysisErrors() const { return analysisErrors_; }
    void printAnalysisErrors(llvm::raw_ostream &os) const;

  private:
    llvm::Function &F_;
    llvm::AAResults &AA_;

    // Placement candidates: globals + allocas
    std::vector<llvm::Value *> candidates_;

    // Candidate globals subset (for AA-based access counting)
    std::vector<llvm::GlobalVariable *> candidateGlobals_;

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
    void computeAccessMaps();
};

} // namespace checkpoint
