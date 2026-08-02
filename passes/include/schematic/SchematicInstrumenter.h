#pragma once

#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include "llvm/IR/Module.h"

#include <map>
#include <vector>

namespace checkpoint {

class SchematicInstrumenter {
  public:
    SchematicInstrumenter(llvm::Module &M, bool addDebugMarkers);

    unsigned instrumentFunction(llvm::Function &F, SchematicSolution &solution,
                                const SchematicStateAnalysis &state);

    /// Per-type insertion counts (populated after instrumentFunction).
    unsigned boundaryCalls() const { return boundaryCalls_; }
    unsigned storeMemCalls() const { return storeMemCalls_; }
    unsigned restoreMemCalls() const { return restoreMemCalls_; }

  private:
    llvm::Module &M_;
    bool addDebugMarkers_;

    llvm::FunctionCallee boundaryFn_;

    /// NVM debug counter globals for memory ops (boundary/reg counters are in assembly)
    llvm::GlobalVariable *cntStoreMemGV_ = nullptr;
    llvm::GlobalVariable *cntRestoreMemGV_ = nullptr;

    /// Per-type insertion counters.
    unsigned boundaryCalls_ = 0;
    unsigned storeMemCalls_ = 0;
    unsigned restoreMemCalls_ = 0;

    /// Shadow globals: original Value (GV or alloca) -> VM shadow GV
    std::map<llvm::Value *, llvm::GlobalVariable *> shadowMap_;

    void declareRuntimeFunctions();

    void createShadowGlobals(llvm::Function &F, const SchematicSolution &solution,
                             const SchematicStateAnalysis &state);

    void dropLifetimeMarkersForShadowedAllocas(llvm::Function &F);

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
