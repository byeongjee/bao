#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/SchematicBlock.h"
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
                 const SchematicStateAnalysis &state, const SchematicParams &params,
                 VMAddressTracker *tracker, SchematicGraph &graph);

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
    VMAddressTracker *tracker_;
    SchematicGraph &graph_;

    std::vector<LoadedLoopTrace> loadedLoopTraces_;

    bool analyzeLoop(llvm::Loop *L, SchematicSolution &solution);
    std::optional<uint64_t> getMaxTripCount(llvm::Loop *L) const;

    RegionAllocation
    buildBoundaryAllocation(const std::map<llvm::Value *, Placement> &placement) const;
};

} // namespace checkpoint
