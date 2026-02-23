#pragma once

#include "common/CFGAnalysis.h"
#include "milp/StateAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"

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

class RCGSolver {
public:
    RCGSolver(const std::vector<llvm::BasicBlock *> &pathBlocks,
              const StateAnalysis &state,
              const CFGAnalysis &cfg,
              const SchematicParams &params,
              const llvm::DenseMap<llvm::BasicBlock *, BlockMetadata> &existingMeta,
              const llvm::DenseMap<llvm::BasicBlock *,
                                   std::map<llvm::GlobalVariable *, Placement>>
                  &decidedPlacements);

    RCGResult solve();

private:
    struct Node {
        enum Kind { Start, CandidateEdge, End };
        Kind kind;
        CFGEdge edge;        // only for CandidateEdge
        unsigned blockIndex; // index into pathBlocks where this edge occurs
    };

    const std::vector<llvm::BasicBlock *> &pathBlocks_;
    const StateAnalysis &state_;
    const CFGAnalysis &cfg_;
    const SchematicParams &params_;
    const llvm::DenseMap<llvm::BasicBlock *, BlockMetadata> &existingMeta_;
    const llvm::DenseMap<llvm::BasicBlock *,
                          std::map<llvm::GlobalVariable *, Placement>>
        &decidedPlacements_;

    std::vector<Node> nodes_;

    void buildNodes();
    double getIntervalBudget(unsigned nodeFrom, unsigned nodeTo) const;

    /// Extract blocks between two nodes (inclusive of interval boundaries).
    std::vector<llvm::BasicBlock *> getIntervalBlocks(unsigned nodeFrom,
                                                       unsigned nodeTo) const;
};

} // namespace checkpoint
