#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"
#include "schematic/TraceLoader.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"

#include <optional>
#include <vector>

namespace checkpoint {

class LoopAnalyzer {
  public:
    LoopAnalyzer(llvm::LoopInfo &LI, llvm::ScalarEvolution &SE, const CFGAnalysis &cfg,
                 const SchematicStateAnalysis &state, const SchematicParams &params);

    /// Set loaded loop traces from TraceLoader for trace-guided analysis.
    void setLoadedLoopTraces(const std::vector<LoadedLoopTrace> &traces);

    /// Returns false if a loop has no trip count annotation (fatal error).
    bool analyzeLoops(SchematicSolution &solution);

  private:
    llvm::LoopInfo &LI_;
    llvm::ScalarEvolution &SE_;
    const CFGAnalysis &cfg_;
    const SchematicStateAnalysis &state_;
    const SchematicParams &params_;

    std::vector<LoadedLoopTrace> loadedLoopTraces_;

    bool analyzeLoop(llvm::Loop *L, SchematicSolution &solution);
    std::optional<uint64_t> getMaxTripCount(llvm::Loop *L) const;

    std::vector<std::vector<llvm::BasicBlock *>>
    enumerateLoopPathsWithoutBackEdges(llvm::Loop *L) const;

    bool placementsDiffer(const std::map<llvm::Value *, Placement> &a,
                          const std::map<llvm::Value *, Placement> &b) const;

    RegionAllocation
    buildBoundaryAllocation(const std::map<llvm::Value *, Placement> &placement) const;

    /// Compute worst-case per-iteration energy via longest path in loop body DAG.
    /// Inner loops with numIterationsPerCharge==0 (entire loop fits in one charge)
    /// are collapsed to E_loop * tripCount to avoid energy underestimation.
    double computeMaxIterationEnergy(llvm::Loop *L, const RegionAllocation &allocation,
                                     const SchematicSolution &solution) const;
};

} // namespace checkpoint
