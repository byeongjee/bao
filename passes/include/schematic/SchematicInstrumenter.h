#pragma once

#include "milp/StateAnalysis.h"
#include "schematic/SchematicSolution.h"

#include "llvm/IR/Module.h"

#include <map>
#include <vector>

namespace checkpoint {

class SchematicInstrumenter {
public:
    SchematicInstrumenter(llvm::Module &M, bool addDebugMarkers, unsigned N_reg);

    unsigned instrumentFunction(llvm::Function &F,
                                const SchematicSolution &solution,
                                const StateAnalysis &state);

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

    /// Shadow globals: original GV -> VM shadow GV
    std::map<llvm::GlobalVariable *, llvm::GlobalVariable *> shadowMap_;
    /// Backups for ineligible memory objects (globals/allocas) that must be
    /// preserved across checkpoints.
    std::map<llvm::Value *, llvm::GlobalVariable *> ineligBackupMap_;
    std::vector<llvm::Value *> ineligCheckpointObjs_;

    void declareRuntimeFunctions();

    void createShadowGlobals(llvm::Function &F,
                             const SchematicSolution &solution,
                             const StateAnalysis &state);
    void createIneligibleBackups(llvm::Function &F, const StateAnalysis &state);

    llvm::BasicBlock *splitEdge(llvm::BasicBlock *src, llvm::BasicBlock *dst);

    unsigned insertCheckpointSequence(llvm::BasicBlock *ckptBB,
                                      const RegionAllocation *endingAlloc,
                                      const RegionAllocation *startingAlloc,
                                      const StateAnalysis &state);

    void rewriteAccessesInRegion(const std::vector<llvm::BasicBlock *> &blocks,
                                 const RegionAllocation &allocation);

    unsigned insertLoopConditionalCheckpoint(
        llvm::BasicBlock *header,
        const LoopCheckpointDecision &decision,
        const StateAnalysis &state);
};

} // namespace checkpoint
