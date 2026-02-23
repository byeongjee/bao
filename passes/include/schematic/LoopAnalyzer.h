#pragma once

#include "common/CFGAnalysis.h"
#include "milp/StateAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include <optional>
#include <vector>

namespace checkpoint {

class LoopAnalyzer {
public:
    LoopAnalyzer(llvm::LoopInfo &LI,
                 llvm::ScalarEvolution &SE,
                 const CFGAnalysis &cfg, const StateAnalysis &state,
                 const SchematicParams &params);

    /// Returns false if a loop has no trip count annotation (fatal error).
    bool analyzeLoops(SchematicSolution &solution);

private:
    llvm::LoopInfo &LI_;
    llvm::ScalarEvolution &SE_;
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const SchematicParams &params_;

    bool analyzeLoop(llvm::Loop *L, SchematicSolution &solution);
    std::optional<uint64_t> getMaxTripCount(llvm::Loop *L) const;

    std::vector<std::vector<llvm::BasicBlock *>>
    enumerateLoopPathsWithoutBackEdges(llvm::Loop *L) const;

    bool placementsDiffer(
        const std::map<llvm::GlobalVariable *, Placement> &a,
        const std::map<llvm::GlobalVariable *, Placement> &b) const;

    RegionAllocation buildBoundaryAllocation(
        const std::map<llvm::GlobalVariable *, Placement> &placement) const;

    /// Compute worst-case per-iteration energy via longest path in loop body DAG.
    /// Inner loops with numIterationsPerCharge==0 (entire loop fits in one charge)
    /// are collapsed to E_loop * tripCount to avoid energy underestimation.
    double computeMaxIterationEnergy(llvm::Loop *L,
                                     const RegionAllocation &allocation,
                                     const SchematicSolution &solution) const;
};

} // namespace checkpoint
