#include "schematic/RCGSolver.h"
#include "common/Logger.h"
#include "schematic/MemoryAllocator.h"

#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <vector>

namespace checkpoint {

RCGSolver::RCGSolver(
    const std::vector<SchematicBlock *> &pathBlocks, const SchematicStateAnalysis &state,
    const CFGAnalysis &cfg, const SchematicParams &params,
    const std::unordered_map<SchematicBlock *, BlockMetadata> &existingMeta,
    const std::unordered_map<SchematicBlock *, std::shared_ptr<RegionAllocation>> &blockAllocation,
    const std::set<SchematicBlock *> &functionCallBlocks, VMAddressTracker *tracker)
    : pathBlocks_(pathBlocks), state_(state), cfg_(cfg), params_(params),
      existingMeta_(existingMeta), blockAllocation_(blockAllocation),
      functionCallBlocks_(functionCallBlocks), tracker_(tracker) {}

void RCGSolver::getCheckpointsFromTrace() {
    nodes_.clear();

    // Start node at index 0 in the path.
    nodes_.push_back(Node{Node::Start, {}, 0});

    // Candidate edge nodes: one for each consecutive pair of blocks.
    for (unsigned i = 0; i + 1 < pathBlocks_.size(); ++i) {
        // Never offer a checkpoint inside an isolated call: the call-interior
        // edge call_entry -> call_exit is skipped (ref: schematic.py:148). The
        // callee's energy already rides on call_entry's folded cost. Node indices
        // stay absolute (blockIndex), so dropping this node is safe for the
        // interval math.
        if (isCallBlock(pathBlocks_[i]) && isCallBlock(pathBlocks_[i + 1]))
            continue;
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
    // Reference: schematic.py:175-179.
    if (pathBlocks_.size() < 3)
        llvm::report_fatal_error("Malformed trace: A trace must be at least 3 basic blocks long");

    unsigned numNodes = nodes_.size();
    adj_.assign(numNodes, {});

    // Reference: schematic.py:183-187 (create_reachable_checkpoint_graph).
    // energy_left = trace[0].energy_left if trace[0].energy_left else (budget - chkpt_restore)
    // energy_to_leave = trace[-1].energy_to_leave if trace[-1].energy_to_leave else chkpt_save
    const RegionAllocation *startAlloc = nullptr;
    const RegionAllocation *endAlloc = nullptr;
    {
        auto it = blockAllocation_.find(pathBlocks_.front());
        if (it != blockAllocation_.end())
            startAlloc = it->second.get();
    }
    {
        auto it = blockAllocation_.find(pathBlocks_.back());
        if (it != blockAllocation_.end())
            endAlloc = it->second.get();
    }

    double energyLeft = params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
    {
        auto it = existingMeta_.find(pathBlocks_.front());
        if (it != existingMeta_.end() && it->second.E_left != std::numeric_limits<double>::max())
            energyLeft = it->second.E_left;
    }
    double energyToLeave = params_.E_epi + params_.N_reg * params_.regStoreEnergy;
    {
        auto it = existingMeta_.find(pathBlocks_.back());
        if (it != existingMeta_.end() && it->second.E_to_leave != 0.0)
            energyToLeave = it->second.E_to_leave;
    }

    // call_cost: the reference subtracts it whenever cfg.first_bb == trace[0]
    // (schematic.py:184-186). That holds for the function cfg (front == START_Func)
    // AND for a loop cfg analyzed via analyse_trace (front == START_Loop). Not-fixed
    // paths front on a fixed real BB, so they keep the full seed. Applied only here
    // in the RCG seed, never in the persistent blockMeta, so it does not leak into
    // propagation reuse (D3).
    if (!pathBlocks_.empty()) {
        llvm::StringRef frontName = pathBlocks_.front()->getName();
        if (frontName == kStartFuncName || frontName == kStartLoopName)
            energyLeft -= params_.callCost;
    }

    // Internal checkpoint indices (all CandidateEdge nodes)
    std::vector<unsigned> internalCkpts;
    for (unsigned i = 1; i + 1 < numNodes; ++i)
        internalCkpts.push_back(i);

    unsigned startNode = 0;
    unsigned endNode = numNodes - 1;

    std::ostringstream pathStream;
    pathStream << "[DEBUG RCG] === createRCG: pathBlocks:";
    for (auto *b : pathBlocks_)
        pathStream << " "
                   << (b->getLLVMBlock() ? b->getLLVMBlock()->getName().str() : b->getName().str());
    PLOGD << pathStream.str();

    PLOGD << "[DEBUG RCG] energyLeft=" << energyLeft << " energyToLeave=" << energyToLeave
          << " startAlloc=" << (startAlloc ? "yes" : "no")
          << " endAlloc=" << (endAlloc ? "yes" : "no");
    if (startAlloc) {
        for (const auto &[v, va] : startAlloc->vars)
            PLOGD << "[DEBUG RCG]   startAlloc var='" << v->getName() << "' "
                  << (va.placement == Placement::VM ? "VM" : "NVM")
                  << " restore=" << va.needRestore() << " save=" << va.needSave();
    }
    if (endAlloc) {
        for (const auto &[v, va] : endAlloc->vars)
            PLOGD << "[DEBUG RCG]   endAlloc var='" << v->getName() << "' "
                  << (va.placement == Placement::VM ? "VM" : "NVM")
                  << " restore=" << va.needRestore() << " save=" << va.needSave();
    }

    // Helper: compute interval cost, handling empty intervals (zero execution cost).
    // Python reference calls compute_cost([]) which returns (MemoryAllocation(), 0).
    // The C++ getIntervalBlocks returns empty when the checkpoint is adjacent to
    // start/end, but we must still create the edge with just overhead costs.
    auto computeIntervalCost = [&](const std::vector<SchematicBlock *> &blocks,
                                   const RegionAllocation *sa,
                                   const RegionAllocation *ea) -> ComputeCostResult {
        if (blocks.empty())
            return {RegionAllocation{}, 0.0};
        return computeCost(blocks, state_, cfg_, params_, blockAllocation_, tracker_, sa, ea);
    };

    // Loop 1: ckpt->ckpt edges (ref: schematic.py:199-218, early termination)
    for (unsigned ii = 0; ii < internalCkpts.size(); ++ii) {
        unsigned i = internalCkpts[ii];
        double prevCost = 0;
        for (unsigned jj = ii + 1; jj < internalCkpts.size(); ++jj) {
            if (prevCost >= params_.capacity)
                break;
            unsigned j = internalCkpts[jj];
            auto blocks = getIntervalBlocks(i, j);
            auto [alloc, cost] = computeIntervalCost(blocks, nullptr, nullptr);
            cost += params_.E_pro + params_.N_reg * params_.regRestoreEnergy; // chkpt_restore
            cost += params_.E_epi + params_.N_reg * params_.regStoreEnergy;   // chkpt_save
            alloc.intervalEnergy = cost;
            trackDiagnostics(blocks, cost, params_.capacity);
            if (cost < params_.capacity)
                adj_[i].push_back({i, j, cost, std::move(alloc), std::move(blocks)});
            prevCost = cost;
        }
    }

    // Loop 2: Start->ckpt edges (ref: schematic.py:236-249, early termination)
    {
        double prevCost = 0;
        for (unsigned jj = 0; jj < internalCkpts.size(); ++jj) {
            if (prevCost >= energyLeft)
                break;
            unsigned j = internalCkpts[jj];
            auto blocks = getIntervalBlocks(startNode, j);
            auto [alloc, cost] = computeIntervalCost(blocks, startAlloc, nullptr);
            cost += params_.E_epi + params_.N_reg * params_.regStoreEnergy; // chkpt_save only
            alloc.intervalEnergy = cost;
            trackDiagnostics(blocks, cost, energyLeft);
            if (cost < energyLeft)
                adj_[startNode].push_back(
                    {startNode, j, cost, std::move(alloc), std::move(blocks)});
            prevCost = cost;
        }
    }

    // Loop 3: ckpt->End edges (ref: schematic.py:254-268, backward + early termination)
    {
        double prevCost = 0;
        for (int ii = static_cast<int>(internalCkpts.size()) - 1; ii >= 0; --ii) {
            if (prevCost + energyToLeave >= params_.capacity)
                break;
            unsigned i = internalCkpts[ii];
            auto blocks = getIntervalBlocks(i, endNode);
            auto [alloc, cost] = computeIntervalCost(blocks, nullptr, endAlloc);
            cost += params_.E_pro + params_.N_reg * params_.regRestoreEnergy; // chkpt_restore only
            alloc.intervalEnergy = cost;
            trackDiagnostics(blocks, cost, params_.capacity - energyToLeave);
            if (cost + energyToLeave < params_.capacity)
                adj_[i].push_back({i, endNode, cost, std::move(alloc), std::move(blocks)});
            prevCost = cost;
        }
    }

    // Loop 4: Start->End edge
    {
        auto blocks = getIntervalBlocks(startNode, endNode);
        auto [alloc, cost] = computeIntervalCost(blocks, startAlloc, endAlloc);
        alloc.intervalEnergy = cost;
        trackDiagnostics(blocks, cost, energyLeft);
        if (cost + energyToLeave < energyLeft)
            adj_[startNode].push_back(
                {startNode, endNode, cost, std::move(alloc), std::move(blocks)});
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
                          "every candidate interval exceeds energy budget";
        if (!minRejectedBlocks_.empty()) {
            msg += " (smallest rejected interval: [";
            for (unsigned i = 0; i < minRejectedBlocks_.size(); ++i) {
                if (i > 0)
                    msg += " -> ";
                auto *b = minRejectedBlocks_[i];
                msg += b->getLLVMBlock() ? b->getLLVMBlock()->getName().str() : b->getName();
            }
            msg += "] requires energy " + std::to_string(minRejectedEnergy_) + " but budget is " +
                   std::to_string(minRejectedBudget_) + ")";
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

        // The "to" node is a CandidateEdge -> it becomes a checkpoint.
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
    // Reference: schematic.py:212,243,261,270 — slicing logic.
    // Start->ckpt: trace[1:j+1], ckpt->End: trace[i:len-1],
    // Start->End: trace[1:len-1], ckpt->ckpt: trace[i+1:j+1].
    unsigned startIdx;
    if (nodes_[nodeFrom].kind == Node::Start) {
        startIdx = 1; // skip trace[0] (ref: trace[1:...])
    } else {
        startIdx = nodes_[nodeFrom].blockIndex;
    }

    unsigned endIdx;
    if (nodes_[nodeTo].kind == Node::End) {
        endIdx =
            static_cast<unsigned>(pathBlocks_.size()) - 2; // skip trace[-1] (ref: trace[:len-1])
    } else {
        endIdx = nodes_[nodeTo].blockIndex - 1;
    }

    return {startIdx, endIdx};
}

std::vector<SchematicBlock *> RCGSolver::getIntervalBlocks(unsigned nodeFrom,
                                                           unsigned nodeTo) const {
    auto [startIdx, endIdx] = getIntervalRange(nodeFrom, nodeTo);
    if (startIdx > endIdx)
        return {};

    return std::vector<SchematicBlock *>(pathBlocks_.begin() + startIdx,
                                         pathBlocks_.begin() + endIdx + 1);
}

void RCGSolver::trackDiagnostics(const std::vector<SchematicBlock *> &blocks, double energy,
                                 double budget) {
    // Track the smallest rejected interval — shows why no RCG edges were created.
    if (energy >= budget && energy < minRejectedEnergy_) {
        minRejectedEnergy_ = energy;
        minRejectedBudget_ = budget;
        minRejectedBlocks_ = blocks;
    }
}

} // namespace checkpoint
