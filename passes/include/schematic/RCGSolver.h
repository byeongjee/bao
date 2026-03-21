#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace checkpoint {

struct RCGResult {
    bool feasible = false;
    double totalCost = 0.0;
    std::vector<CFGEdge> selectedCheckpoints;
    std::vector<RegionAllocation> allocations;
    std::vector<std::vector<SchematicBlock *>> intervalBlocks;
    std::string errorMessage;
};

class VMAddressTracker;

class RCGSolver {
  public:
    RCGSolver(const std::vector<SchematicBlock *> &pathBlocks, const SchematicStateAnalysis &state,
              const CFGAnalysis &cfg, const SchematicParams &params,
              const std::unordered_map<SchematicBlock *, BlockMetadata> &existingMeta,
              const std::unordered_map<SchematicBlock *, std::shared_ptr<RegionAllocation>>
                  &blockAllocation,
              VMAddressTracker *tracker);

    /// Top-level solve: calls the three functions below in sequence.
    RCGResult solve();

  private:
    struct Node {
        enum Kind { Start, CandidateEdge, End };
        Kind kind;
        CFGEdge edge;        // only for CandidateEdge
        unsigned blockIndex; // index into pathBlocks where this edge occurs
    };

    struct RCGEdge {
        unsigned from;
        unsigned to;
        double weight;
        RegionAllocation allocation;
        std::vector<SchematicBlock *> blocks;
    };

    const std::vector<SchematicBlock *> &pathBlocks_;
    const SchematicStateAnalysis &state_;
    const CFGAnalysis &cfg_;
    const SchematicParams &params_;
    const std::unordered_map<SchematicBlock *, BlockMetadata> &existingMeta_;
    const std::unordered_map<SchematicBlock *, std::shared_ptr<RegionAllocation>> &blockAllocation_;
    VMAddressTracker *tracker_;

    std::vector<Node> nodes_;
    std::vector<std::vector<RCGEdge>> adj_;

    // Diagnostic tracking
    double minSingleBlockEnergy_ = std::numeric_limits<double>::infinity();
    double minSingleBlockBudget_ = 0.0;
    SchematicBlock *minSingleBlockBB_ = nullptr;

    /// Reference: get_checkpoints_from_trace — build candidate checkpoint nodes.
    void getCheckpointsFromTrace();

    /// Reference: create_reachable_checkpoint_graph — build RCG edges with 4 loops.
    void createReachableCheckpointGraph();

    /// Reference: get_shortest_path_in_rcg — DP shortest path on DAG.
    RCGResult getShortestPathInRCG();

    std::pair<unsigned, unsigned> getIntervalRange(unsigned nodeFrom, unsigned nodeTo) const;
    std::vector<SchematicBlock *> getIntervalBlocks(unsigned nodeFrom, unsigned nodeTo) const;
    void trackDiagnostics(const std::vector<SchematicBlock *> &blocks, double energy,
                          double budget);
};

} // namespace checkpoint
