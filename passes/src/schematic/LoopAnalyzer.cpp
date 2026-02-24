#include "schematic/LoopAnalyzer.h"

namespace checkpoint {

LoopAnalyzer::LoopAnalyzer(llvm::LoopInfo &LI,
                           llvm::ScalarEvolution &SE,
                           const CFGAnalysis &cfg,
                           const StateAnalysis &state,
                           const SchematicParams &params)
    : LI_(LI), SE_(SE), cfg_(cfg), state_(state), params_(params) {}

bool LoopAnalyzer::analyzeLoops(SchematicSolution &solution) {
    return true;
}

bool LoopAnalyzer::analyzeLoop(llvm::Loop *L, SchematicSolution &solution) {
    return true;
}

std::optional<uint64_t> LoopAnalyzer::getMaxTripCount(llvm::Loop *L) const {
    return std::nullopt;
}

std::vector<std::vector<llvm::BasicBlock *>>
LoopAnalyzer::enumerateLoopPathsWithoutBackEdges(llvm::Loop *L) const {
    return {};
}

bool LoopAnalyzer::placementsDiffer(
    const std::map<llvm::GlobalVariable *, Placement> &a,
    const std::map<llvm::GlobalVariable *, Placement> &b) const {
    return false;
}

RegionAllocation LoopAnalyzer::buildBoundaryAllocation(
    const std::map<llvm::GlobalVariable *, Placement> &placement) const {
    return {};
}

double LoopAnalyzer::computeMaxIterationEnergy(
    llvm::Loop *L, const RegionAllocation &allocation,
    const SchematicSolution &solution) const {
    return 0.0;
}

} // namespace checkpoint
