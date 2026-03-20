#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/IntervalAllocator.h"
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
                 VMAddressTracker *tracker);

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

    std::vector<LoadedLoopTrace> loadedLoopTraces_;

    bool analyzeLoop(llvm::Loop *L, SchematicSolution &solution);
    std::optional<uint64_t> getMaxTripCount(llvm::Loop *L) const;

    RegionAllocation
    buildBoundaryAllocation(const std::map<llvm::Value *, Placement> &placement) const;

    /// Get block energy adjusted for VM placement savings.
    double getAdjustedBlockEnergy(llvm::BasicBlock *BB, const RegionAllocation &alloc) const;

    /// Edge-based energy propagation within a loop (reference: cfg_modification.py:171-317).
    /// Iterates over CFG edges within the loop (including inner loop bodies),
    /// propagating through disabled checkpoint chains only. Uses fixed-point iteration.
    void propagateLoopEnergy(llvm::Loop *L, const RegionAllocation &alloc,
                             SchematicSolution &solution, llvm::BasicBlock *startSynth = nullptr,
                             llvm::BasicBlock *endSynth = nullptr);
};

} // namespace checkpoint
