#include "schematic/RCGSolver.h"
#include "schematic/IntervalAllocator.h"

#include <algorithm>
#include <limits>
#include <vector>

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

void RCGSolver::buildNodes() {
    nodes_.clear();

    // Start node
    Node start;
    start.kind = Node::Start;
    start.edge = {nullptr, nullptr};
    start.blockIndex = 0;
    nodes_.push_back(start);

    // One CandidateEdge node for each consecutive pair in path
    for (unsigned i = 0; i + 1 < pathBlocks_.size(); ++i) {
        Node n;
        n.kind = Node::CandidateEdge;
        n.edge = {pathBlocks_[i], pathBlocks_[i + 1]};
        n.blockIndex = i + 1; // index of the block AFTER the edge
        nodes_.push_back(n);
    }

    // End node
    Node end;
    end.kind = Node::End;
    end.edge = {nullptr, nullptr};
    end.blockIndex = static_cast<unsigned>(pathBlocks_.size());
    nodes_.push_back(end);
}

std::pair<unsigned, unsigned> RCGSolver::getIntervalRange(
    unsigned nodeFrom, unsigned nodeTo) const {

    // Determine block index range
    unsigned fromBlockIdx, toBlockIdx;

    if (nodes_[nodeFrom].kind == Node::Start) {
        fromBlockIdx = 0;
    } else {
        // CandidateEdge: interval starts at the block after the edge
        fromBlockIdx = nodes_[nodeFrom].blockIndex;
    }

    if (nodes_[nodeTo].kind == Node::End) {
        toBlockIdx = static_cast<unsigned>(pathBlocks_.size());
    } else {
        // CandidateEdge: interval includes up to the block before the edge target
        toBlockIdx = nodes_[nodeTo].blockIndex;
    }

    return {fromBlockIdx, toBlockIdx};
}

std::vector<llvm::BasicBlock *> RCGSolver::getIntervalBlocks(
    unsigned nodeFrom, unsigned nodeTo) const {

    auto [fromBlockIdx, toBlockIdx] = getIntervalRange(nodeFrom, nodeTo);
    std::vector<llvm::BasicBlock *> blocks;
    for (unsigned i = fromBlockIdx; i < toBlockIdx; ++i)
        blocks.push_back(pathBlocks_[i]);
    return blocks;
}

double RCGSolver::getIntervalBudget(unsigned nodeFrom,
                                     unsigned nodeTo) const {
    double budget = params_.capacity;

    // Paper overlap rule: for edges from Start, use E_left at boundary.
    if (nodes_[nodeFrom].kind == Node::Start && startBoundaryBlock_) {
        auto it = existingMeta_.find(startBoundaryBlock_);
        if (it != existingMeta_.end())
            budget = std::min(budget, it->second.E_left);
    }

    // Paper overlap rule: for edges to End, use capacity - E_to_leave.
    if (nodes_[nodeTo].kind == Node::End && endBoundaryBlock_) {
        auto it = existingMeta_.find(endBoundaryBlock_);
        if (it != existingMeta_.end())
            budget = std::min(
                budget, params_.capacity - it->second.E_to_leave);
    }

    return budget;
}

RCGResult RCGSolver::solve() {
    RCGResult result;
    buildNodes();

    unsigned numNodes = nodes_.size();
    if (numNodes < 2) {
        result.feasible = false;
        result.errorMessage = "Path too short for RCG analysis";
        return result;
    }

    // DP shortest path on the DAG (nodes are in topological order)
    constexpr double INF = std::numeric_limits<double>::max();
    std::vector<double> dist(numNodes, INF);
    std::vector<int> pred(numNodes, -1);

    // Per-edge data: (from, to) -> allocation and interval blocks.
    // O(N^2) storage where N = number of RCG nodes (path length + 2).
    // Acceptable for typical paths (tens to low hundreds of blocks).
    // For very long paths, consider switching to a sparse map.
    struct EdgeData {
        RegionAllocation allocation;
        std::vector<llvm::BasicBlock *> blocks;
    };
    std::vector<std::vector<EdgeData>> edgeData(numNodes);
    for (auto &v : edgeData)
        v.resize(numNodes);

    dist[0] = 0.0; // Start node

    // Build edges and compute DP
    for (unsigned i = 0; i < numNodes; ++i) {
        if (dist[i] == INF) continue;

        for (unsigned j = i + 1; j < numNodes; ++j) {
            auto blocks = getIntervalBlocks(i, j);
            if (blocks.empty()) continue;

            bool isFirst = (nodes_[i].kind == Node::Start);
            bool isLast = (nodes_[j].kind == Node::End);

            auto [fromIdx, toIdx] = getIntervalRange(i, j);
            (void)fromIdx;
            std::vector<llvm::BasicBlock *> postBlocks;
            for (unsigned k = toIdx; k < pathBlocks_.size(); ++k)
                postBlocks.push_back(pathBlocks_[k]);

            // Collect fixed placements from previously decided blocks
            std::map<llvm::GlobalVariable *, Placement> fixedPlacements;
            for (llvm::BasicBlock *BB : blocks) {
                auto it = decidedPlacements_.find(BB);
                if (it != decidedPlacements_.end()) {
                    for (const auto &[gv, p] : it->second)
                        fixedPlacements.insert({gv, p}); // first decision wins
                }
            }

            auto allocation = computeIntervalAllocation(
                blocks, state_, params_, fixedPlacements, &postBlocks);
            double energy = computeIntervalEnergy(
                blocks, allocation, state_, cfg_, params_,
                isFirst, isLast, &postBlocks);

            double budget = getIntervalBudget(i, j);

            if (energy <= budget) {
                double newDist = dist[i] + energy;
                if (newDist < dist[j]) {
                    dist[j] = newDist;
                    pred[j] = static_cast<int>(i);
                    edgeData[i][j].allocation = std::move(allocation);
                    edgeData[i][j].blocks = std::move(blocks);
                }
            }
        }
    }

    // Check feasibility
    unsigned endIdx = numNodes - 1;
    if (dist[endIdx] == INF) {
        result.feasible = false;
        result.errorMessage = "No feasible checkpoint partition exists for this path";
        return result;
    }

    // Reconstruct path
    std::vector<unsigned> rcgPath;
    for (int n = static_cast<int>(endIdx); n >= 0; n = pred[n]) {
        rcgPath.push_back(static_cast<unsigned>(n));
    }
    std::reverse(rcgPath.begin(), rcgPath.end());

    // Extract results
    result.feasible = true;
    result.totalCost = dist[endIdx];

    for (size_t k = 0; k + 1 < rcgPath.size(); ++k) {
        unsigned from = rcgPath[k];
        unsigned to = rcgPath[k + 1];

        result.intervalBlocks.push_back(edgeData[from][to].blocks);
        result.allocations.push_back(std::move(edgeData[from][to].allocation));

        // Record checkpoint edges (skip Start and End nodes)
        if (nodes_[from].kind == Node::CandidateEdge) {
            result.selectedCheckpoints.push_back(nodes_[from].edge);
        }
    }
    // Also check if the last RCG node before End is a CandidateEdge
    // (already handled by the loop above — the edge AT the node is the checkpoint)

    return result;
}

} // namespace checkpoint
