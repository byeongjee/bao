#include "schematic/IntervalAllocator.h"

namespace checkpoint {

std::pair<bool, bool> computeLivenessFlags(
    llvm::GlobalVariable *v,
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const StateAnalysis &state,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {
    return {false, false};
}

RegionAllocation computeIntervalAllocation(
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const StateAnalysis &state,
    const SchematicParams &params,
    const std::map<llvm::GlobalVariable *, Placement> &fixedPlacements,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {
    return {};
}

double computeIntervalEnergy(
    const std::vector<llvm::BasicBlock *> &intervalBlocks,
    const RegionAllocation &allocation,
    const StateAnalysis &state,
    const CFGAnalysis &cfg,
    const SchematicParams &params,
    bool isFirstInterval,
    bool isLastInterval,
    const std::vector<llvm::BasicBlock *> *postIntervalBlocks) {
    return 0.0;
}

} // namespace checkpoint
