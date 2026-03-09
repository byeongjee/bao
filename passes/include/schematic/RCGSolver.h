#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include "llvm/ADT/DenseMap.h"

#include <map>
#include <string>
#include <vector>

namespace checkpoint {

struct RCGResult {
    bool feasible = false;
    double totalCost = 0.0;
    std::vector<CFGEdge> selectedCheckpoints;
    std::vector<RegionAllocation> allocations;
    std::vector<std::vector<llvm::BasicBlock *>> intervalBlocks;
    std::string errorMessage;
};

class VMAddressTracker;

class RCGSolver {
  public:
    RCGSolver(const std::vector<llvm::BasicBlock *> &pathBlocks,
              const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
              const SchematicParams &params,
              const llvm::DenseMap<llvm::BasicBlock *, BlockMetadata> &existingMeta,
              const llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>>
                  &decidedPlacements,
              llvm::BasicBlock *startBoundaryBlock, llvm::BasicBlock *endBoundaryBlock,
              VMAddressTracker *tracker);

    RCGResult solve();

  private:
    struct Node {
        enum Kind { Start, CandidateEdge, End };
        Kind kind;
        CFGEdge edge;        // only for CandidateEdge
        unsigned blockIndex; // index into pathBlocks where this edge occurs
    };

    const std::vector<llvm::BasicBlock *> &pathBlocks_;
    const SchematicStateAnalysis &state_;
    const CFGAnalysis &cfg_;
    const SchematicParams &params_;
    const llvm::DenseMap<llvm::BasicBlock *, BlockMetadata> &existingMeta_;
    const llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>>
        &decidedPlacements_;
    llvm::BasicBlock *startBoundaryBlock_;
    llvm::BasicBlock *endBoundaryBlock_;
    VMAddressTracker *tracker_;

    std::vector<Node> nodes_;

    void buildNodes();
    double getIntervalBudget(unsigned nodeFrom, unsigned nodeTo) const;

    std::pair<unsigned, unsigned> getIntervalRange(unsigned nodeFrom, unsigned nodeTo) const;

    /// Extract blocks between two nodes (inclusive of interval boundaries).
    std::vector<llvm::BasicBlock *> getIntervalBlocks(unsigned nodeFrom, unsigned nodeTo) const;
};

} // namespace checkpoint
