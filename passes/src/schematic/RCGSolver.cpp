#include "schematic/RCGSolver.h"
#include "schematic/IntervalAllocator.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace checkpoint {

RCGSolver::RCGSolver(
    const std::vector<llvm::BasicBlock *> &pathBlocks, const SchematicStateAnalysis &state,
    const CFGAnalysis &cfg, const SchematicParams &params,
    const llvm::DenseMap<llvm::BasicBlock *, BlockMetadata> &existingMeta,
    const llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>> &decidedPlacements,
    llvm::BasicBlock *startBoundaryBlock, llvm::BasicBlock *endBoundaryBlock,
    VMAddressTracker *tracker)
    : pathBlocks_(pathBlocks), state_(state), cfg_(cfg), params_(params),
      existingMeta_(existingMeta), decidedPlacements_(decidedPlacements),
      startBoundaryBlock_(startBoundaryBlock), endBoundaryBlock_(endBoundaryBlock),
      tracker_(tracker) {}

void RCGSolver::buildNodes() {
    nodes_.clear();

    // Start node at index 0 in the path.
    nodes_.push_back(Node{Node::Start, {}, 0});

    // Candidate edge nodes: one for each consecutive pair of blocks.
    for (unsigned i = 0; i + 1 < pathBlocks_.size(); ++i) {
        Node n;
        n.kind = Node::CandidateEdge;
        n.edge = CFGEdge{pathBlocks_[i], pathBlocks_[i + 1]};
        n.blockIndex = i + 1; // index of the block after the edge
        nodes_.push_back(n);
    }

    // End node.
    nodes_.push_back(Node{Node::End, {}, static_cast<unsigned>(pathBlocks_.size())});
}

double RCGSolver::getIntervalBudget(unsigned nodeFrom, unsigned nodeTo) const {
    double budget = params_.capacity;

    bool isStart = (nodeFrom == 0);
    bool isEnd = (nodeTo == nodes_.size() - 1);

    double E_left_budget = params_.capacity;
    double E_to_leave_budget = params_.capacity;

    if (isStart && startBoundaryBlock_) {
        auto it = existingMeta_.find(startBoundaryBlock_);
        if (it != existingMeta_.end()) {
            E_left_budget = it->second.E_left;
        }
    }

    if (isEnd && endBoundaryBlock_) {
        auto it = existingMeta_.find(endBoundaryBlock_);
        if (it != existingMeta_.end()) {
            E_to_leave_budget = params_.capacity - it->second.E_to_leave;
        }
    }

    if (isStart && startBoundaryBlock_ && isEnd && endBoundaryBlock_) {
        budget = std::min(E_left_budget, E_to_leave_budget);
    } else if (isStart && startBoundaryBlock_) {
        budget = E_left_budget;
    } else if (isEnd && endBoundaryBlock_) {
        budget = E_to_leave_budget;
    }

    return budget;
}

std::pair<unsigned, unsigned> RCGSolver::getIntervalRange(unsigned nodeFrom,
                                                          unsigned nodeTo) const {
    unsigned startIdx;
    if (nodes_[nodeFrom].kind == Node::Start) {
        startIdx = 0;
    } else {
        // CandidateEdge: interval begins at the block after the edge.
        startIdx = nodes_[nodeFrom].blockIndex;
    }

    unsigned endIdx;
    if (nodes_[nodeTo].kind == Node::End) {
        endIdx = static_cast<unsigned>(pathBlocks_.size()) - 1;
    } else {
        // CandidateEdge: interval ends at the block before the edge.
        endIdx = nodes_[nodeTo].blockIndex - 1;
    }

    return {startIdx, endIdx};
}

std::vector<llvm::BasicBlock *> RCGSolver::getIntervalBlocks(unsigned nodeFrom,
                                                             unsigned nodeTo) const {
    auto [startIdx, endIdx] = getIntervalRange(nodeFrom, nodeTo);
    if (startIdx > endIdx)
        return {};

    return std::vector<llvm::BasicBlock *>(pathBlocks_.begin() + startIdx,
                                           pathBlocks_.begin() + endIdx + 1);
}

RCGResult RCGSolver::solve() {
    RCGResult result;

    if (pathBlocks_.empty()) {
        result.feasible = true;
        result.totalCost = 0.0;
        return result;
    }

    buildNodes();

    unsigned numNodes = nodes_.size();

    // Build start/end constraint allocations from boundary blocks.
    std::optional<RegionAllocation> startConstraintAlloc;
    std::optional<RegionAllocation> endConstraintAlloc;
    if (startBoundaryBlock_) {
        auto it = decidedPlacements_.find(startBoundaryBlock_);
        if (it != decidedPlacements_.end()) {
            RegionAllocation a;
            a.placement = it->second;
            startConstraintAlloc = std::move(a);
        }
    }
    if (endBoundaryBlock_) {
        auto it = decidedPlacements_.find(endBoundaryBlock_);
        if (it != decidedPlacements_.end()) {
            RegionAllocation a;
            a.placement = it->second;
            endConstraintAlloc = std::move(a);
        }
    }

    // Single-interval shortcut: Start + End only, no candidate edges.
    if (numNodes == 2) {
        auto blocks = getIntervalBlocks(0, 1);
        std::map<llvm::Value *, Placement> fixed;
        for (llvm::BasicBlock *BB : blocks) {
            auto it = decidedPlacements_.find(BB);
            if (it != decidedPlacements_.end()) {
                for (const auto &[gv, place] : it->second)
                    fixed[gv] = place;
            }
        }
        const RegionAllocation *sc = startConstraintAlloc ? &*startConstraintAlloc : nullptr;
        const RegionAllocation *ec = endConstraintAlloc ? &*endConstraintAlloc : nullptr;
        auto alloc = computeIntervalAllocation(blocks, state_, params_, fixed, tracker_, sc, ec);
        double energy = computeIntervalEnergy(blocks, alloc, state_, cfg_, params_, true, true);
        alloc.intervalEnergy = energy;
        double budget = getIntervalBudget(0, 1);

        if (energy <= budget) {
            result.feasible = true;
            result.totalCost = energy;
            result.allocations.push_back(std::move(alloc));
            result.intervalBlocks.push_back(std::move(blocks));
            return result;
        }
        // Fall through to full RCG construction.
    }

    // Build RCG edges: for all i < j, check if interval (i,j) is feasible.
    struct RCGEdge {
        unsigned from;
        unsigned to;
        double weight;
        RegionAllocation allocation;
        std::vector<llvm::BasicBlock *> blocks;
    };

    // Adjacency list.
    std::vector<std::vector<RCGEdge>> adj(numNodes);

    // Track the smallest single-block interval energy and its budget for diagnostics.
    double minSingleBlockEnergy = std::numeric_limits<double>::infinity();
    double minSingleBlockBudget = 0.0;
    llvm::BasicBlock *minSingleBlockBB = nullptr;

    for (unsigned i = 0; i < numNodes; ++i) {
        for (unsigned j = i + 1; j < numNodes; ++j) {
            auto blocks = getIntervalBlocks(i, j);
            if (blocks.empty())
                continue;

            // Collect fixed placements for these blocks.
            std::map<llvm::Value *, Placement> fixed;
            for (llvm::BasicBlock *BB : blocks) {
                auto it = decidedPlacements_.find(BB);
                if (it != decidedPlacements_.end()) {
                    for (const auto &[gv, place] : it->second)
                        fixed[gv] = place;
                }
            }

            bool isFirst = (i == 0);
            bool isLast = (j == numNodes - 1);
            const RegionAllocation *sc =
                (isFirst && startConstraintAlloc) ? &*startConstraintAlloc : nullptr;
            const RegionAllocation *ec =
                (isLast && endConstraintAlloc) ? &*endConstraintAlloc : nullptr;
            auto alloc =
                computeIntervalAllocation(blocks, state_, params_, fixed, tracker_, sc, ec);
            double energy =
                computeIntervalEnergy(blocks, alloc, state_, cfg_, params_, isFirst, isLast);
            alloc.intervalEnergy = energy;
            double budget = getIntervalBudget(i, j);

            if (energy <= budget) {
                adj[i].push_back(RCGEdge{i, j, energy, std::move(alloc), std::move(blocks)});
            }

            // Track the minimum single-block interval for diagnostics.
            if (blocks.size() == 1 && energy < minSingleBlockEnergy) {
                minSingleBlockEnergy = energy;
                minSingleBlockBudget = budget;
                minSingleBlockBB = blocks[0];
            }
        }
    }

    // DP shortest path on DAG.
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> dist(numNodes, INF);
    std::vector<int> pred(numNodes, -1);
    std::vector<int> predEdgeIdx(numNodes, -1);
    dist[0] = 0.0;

    for (unsigned i = 0; i < numNodes; ++i) {
        if (dist[i] == INF)
            continue;
        for (unsigned e = 0; e < adj[i].size(); ++e) {
            const auto &edge = adj[i][e];
            double newDist = dist[i] + edge.weight;
            if (newDist < dist[edge.to]) {
                dist[edge.to] = newDist;
                pred[edge.to] = static_cast<int>(i);
                predEdgeIdx[edge.to] = static_cast<int>(e);
            }
        }
    }

    unsigned endNode = numNodes - 1;
    if (dist[endNode] == INF) {
        result.feasible = false;
        std::string msg = "SCHEMATIC RCG: no feasible path from Start to End — "
                          "energy capacity too small for this path";
        if (minSingleBlockBB) {
            msg += " (smallest single-block interval: block '" + minSingleBlockBB->getName().str() +
                   "' requires energy " + std::to_string(minSingleBlockEnergy) + " but budget is " +
                   std::to_string(minSingleBlockBudget) + ")";
        }
        result.errorMessage = msg;
        return result;
    }

    // Backtrack to collect path.
    std::vector<unsigned> pathNodes;
    for (int n = static_cast<int>(endNode); n >= 0; n = pred[n]) {
        pathNodes.push_back(static_cast<unsigned>(n));
    }
    std::reverse(pathNodes.begin(), pathNodes.end());

    // Collect results: checkpoints, allocations, interval blocks.
    result.feasible = true;
    result.totalCost = dist[endNode];

    for (unsigned k = 0; k + 1 < pathNodes.size(); ++k) {
        unsigned from = pathNodes[k];
        unsigned to = pathNodes[k + 1];
        int eIdx = predEdgeIdx[to];
        const auto &edge = adj[from][eIdx];

        result.allocations.push_back(edge.allocation);
        result.intervalBlocks.push_back(edge.blocks);

        // The "to" node is a CandidateEdge → it becomes a checkpoint.
        // But the last node (End) is not a checkpoint.
        if (nodes_[to].kind == Node::CandidateEdge) {
            result.selectedCheckpoints.push_back(nodes_[to].edge);
        }
    }

    return result;
}

} // namespace checkpoint
