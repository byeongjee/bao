#include "schematic/SchematicInstrumenter.h"

namespace checkpoint {

SchematicInstrumenter::SchematicInstrumenter(llvm::Module &M,
                                             bool addDebugMarkers,
                                             unsigned N_reg)
    : M_(M), addDebugMarkers_(addDebugMarkers), N_reg_(N_reg) {}

void SchematicInstrumenter::declareRuntimeFunctions() {}

void SchematicInstrumenter::createShadowGlobals(
    llvm::Function &F,
    const SchematicSolution &solution,
    const StateAnalysis &state) {}

void SchematicInstrumenter::createIneligibleBackups(
    llvm::Function &F,
    const StateAnalysis &state) {}

llvm::BasicBlock *SchematicInstrumenter::splitEdge(llvm::BasicBlock *src,
                                                    llvm::BasicBlock *dst) {
    return nullptr;
}

void SchematicInstrumenter::rewriteAccessesInRegion(
    const std::vector<llvm::BasicBlock *> &blocks,
    const RegionAllocation &allocation) {}

unsigned SchematicInstrumenter::insertCheckpointSequence(
    llvm::BasicBlock *ckptBB,
    const RegionAllocation *endingAlloc,
    const RegionAllocation *startingAlloc,
    const StateAnalysis &state) {
    return 0;
}

unsigned SchematicInstrumenter::insertLoopConditionalCheckpoint(
    llvm::BasicBlock *header,
    const LoopCheckpointDecision &decision,
    const StateAnalysis &state) {
    return 0;
}

unsigned SchematicInstrumenter::instrumentFunction(
    llvm::Function &F,
    const SchematicSolution &solution,
    const StateAnalysis &state) {
    return 0;
}

} // namespace checkpoint
