#include "schematic/RCGSolver.h"
#include "schematic/MemoryAllocator.h"

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

void RCGSolver::getCheckpointsFromTrace() {
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

void RCGSolver::createReachableCheckpointGraph() {
    unsigned numNodes = nodes_.size();
    adj_.assign(numNodes, {});

    // Build constraint allocations from boundary blocks.
    std::optional<RegionAllocation> startConstraintAlloc;
    std::optional<RegionAllocation> endConstraintAlloc;
    if (startBoundaryBlock_) {
        auto it = decidedPlacements_.find(startBoundaryBlock_);
        if (it != decidedPlacements_.end()) {
            RegionAllocation a;
            for (const auto &[v, place] : it->second)
                a.vars[v].placement = place;
            startConstraintAlloc = std::move(a);
        }
    }
    if (endBoundaryBlock_) {
        auto it = decidedPlacements_.find(endBoundaryBlock_);
        if (it != decidedPlacements_.end()) {
            RegionAllocation a;
            for (const auto &[v, place] : it->second)
                a.vars[v].placement = place;
            endConstraintAlloc = std::move(a);
        }
    }

    // Read boundary energy values
    double energyLeft = params_.capacity;
    if (startBoundaryBlock_) {
        auto it = existingMeta_.find(startBoundaryBlock_);
        if (it != existingMeta_.end())
            energyLeft = it->second.E_left;
    } else {
        energyLeft = params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
    }
    double energyToLeave = 0.0;
    if (endBoundaryBlock_) {
        auto it = existingMeta_.find(endBoundaryBlock_);
        if (it != existingMeta_.end())
            energyToLeave = it->second.E_to_leave;
    }

    // Helper to collect fixed placements for a block range
    auto collectFixed = [&](const std::vector<llvm::BasicBlock *> &blocks) {
        std::map<llvm::Value *, Placement> fixed;
        for (llvm::BasicBlock *BB : blocks) {
            auto it = decidedPlacements_.find(BB);
            if (it != decidedPlacements_.end())
                for (const auto &[gv, place] : it->second)
                    fixed[gv] = place;
        }
        return fixed;
    };

    // Internal checkpoint indices (all CandidateEdge nodes)
    std::vector<unsigned> internalCkpts;
    for (unsigned i = 1; i + 1 < numNodes; ++i)
        internalCkpts.push_back(i);

    unsigned startNode = 0;
    unsigned endNode = numNodes - 1;

    // Loop 1: ckpt→ckpt edges
    for (unsigned ii = 0; ii < internalCkpts.size(); ++ii) {
        for (unsigned jj = ii + 1; jj < internalCkpts.size(); ++jj) {
            unsigned i = internalCkpts[ii];
            unsigned j = internalCkpts[jj];
            auto blocks = getIntervalBlocks(i, j);
            if (blocks.empty())
                continue;
            auto fixed = collectFixed(blocks);
            auto [alloc, cost] =
                computeCost(blocks, state_, cfg_, params_, fixed, tracker_, nullptr, nullptr);
            cost += params_.E_pro + params_.N_reg * params_.regRestoreEnergy; // chkpt_restore
            cost += params_.E_epi + params_.N_reg * params_.regStoreEnergy;   // chkpt_save
            alloc.intervalEnergy = cost;
            trackDiagnostics(blocks, cost, params_.capacity);
            if (cost < params_.capacity)
                adj_[i].push_back({i, j, cost, std::move(alloc), std::move(blocks)});
        }
    }

    // Loop 2: Start→ckpt edges
    for (unsigned jj = 0; jj < internalCkpts.size(); ++jj) {
        unsigned j = internalCkpts[jj];
        auto blocks = getIntervalBlocks(startNode, j);
        if (blocks.empty())
            continue;
        auto fixed = collectFixed(blocks);
        const RegionAllocation *sc = startConstraintAlloc ? &*startConstraintAlloc : nullptr;
        auto [alloc, cost] =
            computeCost(blocks, state_, cfg_, params_, fixed, tracker_, sc, nullptr);
        cost += params_.E_epi + params_.N_reg * params_.regStoreEnergy; // chkpt_save only
        alloc.intervalEnergy = cost;
        trackDiagnostics(blocks, cost, energyLeft);
        if (cost < energyLeft)
            adj_[startNode].push_back({startNode, j, cost, std::move(alloc), std::move(blocks)});
    }

    // Loop 3: ckpt→End edges
    for (unsigned ii = 0; ii < internalCkpts.size(); ++ii) {
        unsigned i = internalCkpts[ii];
        auto blocks = getIntervalBlocks(i, endNode);
        if (blocks.empty())
            continue;
        auto fixed = collectFixed(blocks);
        const RegionAllocation *ec = endConstraintAlloc ? &*endConstraintAlloc : nullptr;
        auto [alloc, cost] =
            computeCost(blocks, state_, cfg_, params_, fixed, tracker_, nullptr, ec);
        cost += params_.E_pro + params_.N_reg * params_.regRestoreEnergy; // chkpt_restore only
        alloc.intervalEnergy = cost;
        trackDiagnostics(blocks, cost, params_.capacity - energyToLeave);
        if (cost + energyToLeave < params_.capacity)
            adj_[i].push_back({i, endNode, cost, std::move(alloc), std::move(blocks)});
    }

    // Loop 4: Start→End edge
    {
        auto blocks = getIntervalBlocks(startNode, endNode);
        if (!blocks.empty()) {
            auto fixed = collectFixed(blocks);
            const RegionAllocation *sc = startConstraintAlloc ? &*startConstraintAlloc : nullptr;
            const RegionAllocation *ec = endConstraintAlloc ? &*endConstraintAlloc : nullptr;
            auto [alloc, cost] =
                computeCost(blocks, state_, cfg_, params_, fixed, tracker_, sc, ec);
            alloc.intervalEnergy = cost;
            trackDiagnostics(blocks, cost, energyLeft);
            if (cost + energyToLeave < energyLeft)
                adj_[startNode].push_back(
                    {startNode, endNode, cost, std::move(alloc), std::move(blocks)});
        }
    }
}

RCGResult RCGSolver::getShortestPathInRCG() {
    RCGResult result;
    unsigned numNodes = nodes_.size();
    unsigned endNode = numNodes - 1;

    // DP shortest path on DAG.
    const double INF = std::numeric_limits<double>::infinity();
    std::vector<double> dist(numNodes, INF);
    std::vector<int> pred(numNodes, -1);
    std::vector<int> predEdgeIdx(numNodes, -1);
    dist[0] = 0.0;

    for (unsigned i = 0; i < numNodes; ++i) {
        if (dist[i] == INF)
            continue;
        for (unsigned e = 0; e < adj_[i].size(); ++e) {
            const auto &edge = adj_[i][e];
            double newDist = dist[i] + edge.weight;
            if (newDist < dist[edge.to]) {
                dist[edge.to] = newDist;
                pred[edge.to] = static_cast<int>(i);
                predEdgeIdx[edge.to] = static_cast<int>(e);
            }
        }
    }

    if (dist[endNode] == INF) {
        result.feasible = false;
        std::string msg = "SCHEMATIC RCG: no feasible path from Start to End — "
                          "energy capacity too small for this path";
        if (minSingleBlockBB_) {
            msg += " (smallest single-block interval: block '" +
                   minSingleBlockBB_->getName().str() + "' requires energy " +
                   std::to_string(minSingleBlockEnergy_) + " but budget is " +
                   std::to_string(minSingleBlockBudget_) + ")";
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
        const auto &edge = adj_[from][eIdx];

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

RCGResult RCGSolver::solve() {
    if (pathBlocks_.empty()) {
        RCGResult result;
        result.feasible = true;
        result.totalCost = 0.0;
        return result;
    }
    getCheckpointsFromTrace();
    createReachableCheckpointGraph();
    return getShortestPathInRCG();
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

void RCGSolver::trackDiagnostics(const std::vector<llvm::BasicBlock *> &blocks, double energy,
                                 double budget) {
    if (blocks.size() == 1 && energy < minSingleBlockEnergy_) {
        minSingleBlockEnergy_ = energy;
        minSingleBlockBudget_ = budget;
        minSingleBlockBB_ = blocks[0];
    }
}

} // namespace checkpoint
