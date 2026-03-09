#pragma once

#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include "llvm/IR/Module.h"

#include <map>
#include <vector>

namespace checkpoint {

class SchematicInstrumenter {
  public:
    SchematicInstrumenter(llvm::Module &M, bool addDebugMarkers, unsigned N_reg);

    unsigned instrumentFunction(llvm::Function &F, const SchematicSolution &solution,
                                const SchematicStateAnalysis &state);

  private:
    llvm::Module &M_;
    bool addDebugMarkers_;
    unsigned N_reg_;

    llvm::FunctionCallee prologueFn_;
    llvm::FunctionCallee epilogueFn_;
    llvm::FunctionCallee storeMemFn_;
    llvm::FunctionCallee restoreMemFn_;
    llvm::FunctionCallee storeRegFn_;
    llvm::FunctionCallee restoreRegFn_;

    /// Shadow globals: original Value (GV or alloca) -> VM shadow GV
    std::map<llvm::Value *, llvm::GlobalVariable *> shadowMap_;
    /// Backups for ineligible objects (cross-block SSA values only).
    std::map<llvm::Value *, llvm::GlobalVariable *> ineligBackupMap_;
    std::vector<llvm::Value *> ineligCheckpointObjs_;

    void declareRuntimeFunctions();

    void createShadowGlobals(llvm::Function &F, const SchematicSolution &solution,
                             const SchematicStateAnalysis &state);
    void createIneligibleBackups(llvm::Function &F, const SchematicStateAnalysis &state);

    llvm::BasicBlock *splitEdge(llvm::BasicBlock *src, llvm::BasicBlock *dst);

    unsigned insertCheckpointSequence(llvm::BasicBlock *ckptBB, const RegionAllocation *endingAlloc,
                                      const RegionAllocation *startingAlloc,
                                      const SchematicStateAnalysis &state);

    void rewriteAccessesInRegion(const std::vector<llvm::BasicBlock *> &blocks,
                                 const RegionAllocation &allocation);

    unsigned insertLoopConditionalCheckpoint(llvm::BasicBlock *header,
                                             const LoopCheckpointDecision &decision,
                                             const SchematicStateAnalysis &state);
};

} // namespace checkpoint
