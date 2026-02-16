#pragma once

#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
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
class StateAnalysis {
public:
    StateAnalysis(llvm::Function &F,
                  llvm::AAResults &AA,
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

    // -- Access maps (for energy model) --

    /// Number of loads from global v in block b.
    unsigned getLoadCount(const std::string &block,
                          llvm::GlobalVariable *gv) const;

    /// Number of stores to global v in block b.
    unsigned getStoreCount(const std::string &block,
                           llvm::GlobalVariable *gv) const;

    // -- Mappings --

    /// Get candidate global size in bytes (0 for non-candidates/unknown).
    unsigned getVMObjSizeBytes(llvm::GlobalVariable *gv) const;

    /// Get the LLVM BasicBlock* for a block name.
    llvm::BasicBlock *getBlock(const std::string &name) const;

private:
    llvm::Function &F_;
    llvm::AAResults &AA_;
    const CFGAnalysis &cfg_;

    // Candidate globals
    std::vector<llvm::GlobalVariable *> vmObjs_;
    std::set<llvm::GlobalVariable *> vmObjSet_;
    std::vector<std::string> analysisErrors_;
    std::map<llvm::GlobalVariable *, unsigned> vmObjSizeBytes_;

    // Liveness
    std::map<std::string, std::set<llvm::GlobalVariable *>> vmObjLiveIn_;
    std::map<std::string, std::set<llvm::GlobalVariable *>> defGlobals_;

    // Access maps: (block, globalVar) -> count
    std::map<std::pair<std::string, llvm::GlobalVariable *>, unsigned> loadCounts_;
    std::map<std::pair<std::string, llvm::GlobalVariable *>, unsigned> storeCounts_;

    // Block name -> BasicBlock mapping
    std::map<std::string, llvm::BasicBlock *> nameToBlock_;

    // Empty sets for returning references
    static const std::set<llvm::GlobalVariable *> emptyGVSet_;

    bool isMilpCandidateAnnotated(llvm::GlobalVariable *GV) const;
    bool isAllowedDirectCall(const llvm::CallBase &CB) const;
    bool validateInstructionForStrictMode(const llvm::Instruction &I);
    void reportStrictError(const llvm::Instruction &I, const std::string &reason);

    void identifyVMObjs();
    void buildBlockMap();
    void computeAccessMaps();
    void computeVMObjLiveness();
};

} // namespace checkpoint
