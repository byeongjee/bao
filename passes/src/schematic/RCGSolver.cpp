#include "schematic/RCGSolver.h"

namespace checkpoint {

RCGSolver::RCGSolver(
    const std::vector<llvm::BasicBlock *> &pathBlocks,
    const StateAnalysis &state,
    const CFGAnalysis &cfg,
    const SchematicParams &params,
    const llvm::DenseMap<llvm::BasicBlock *, BlockMetadata> &existingMeta,
    const llvm::DenseMap<llvm::BasicBlock *,
                         std::map<llvm::GlobalVariable *, Placement>>
        &decidedPlacements,
    llvm::BasicBlock *startBoundaryBlock,
    llvm::BasicBlock *endBoundaryBlock)
    : pathBlocks_(pathBlocks), state_(state), cfg_(cfg), params_(params),
      existingMeta_(existingMeta), decidedPlacements_(decidedPlacements),
      startBoundaryBlock_(startBoundaryBlock), endBoundaryBlock_(endBoundaryBlock) {}

RCGResult RCGSolver::solve() {
    RCGResult result;
    result.feasible = false;
    result.errorMessage = "SCHEMATIC RCG solver not yet implemented";
    return result;
}

void RCGSolver::buildNodes() {}

double RCGSolver::getIntervalBudget(unsigned nodeFrom,
                                     unsigned nodeTo) const {
    return 0.0;
}

std::pair<unsigned, unsigned> RCGSolver::getIntervalRange(
    unsigned nodeFrom, unsigned nodeTo) const {
    return {0, 0};
}

std::vector<llvm::BasicBlock *> RCGSolver::getIntervalBlocks(
    unsigned nodeFrom, unsigned nodeTo) const {
    return {};
}

} // namespace checkpoint
