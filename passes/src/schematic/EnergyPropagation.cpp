// passes/src/schematic/EnergyPropagation.cpp
#include "schematic/EnergyPropagation.h"

#include <deque>
#include <map>
#include <set>

namespace checkpoint {

double getBlockExecEnergy(SchematicBlock *block, const SchematicSolution &solution,
                          const CFGAnalysis &cfg, const SchematicStateAnalysis &state,
                          const SchematicParams &params) {
    auto *BB = block->getLLVMBlock();
    if (!BB)
        return 0.0;
    double E = cfg.getBlockInfo(BB).energyCost;
    auto allocIt = solution.decidedPlacements.find(block);
    if (allocIt != solution.decidedPlacements.end()) {
        for (const auto &[gv, place] : allocIt->second) {
            if (place != Placement::VM)
                continue;
            unsigned loads = state.getLoadCount(BB, gv);
            unsigned stores = state.getStoreCount(BB, gv);
            E -= params.nvmAccessPenalty * (loads + stores);
        }
    }
    return E;
}

void propagateEnergyToLeave(const CFGEdge &seedEdge, double seedEToLeave,
                            SchematicSolution &solution, const CFGAnalysis &cfg,
                            const SchematicStateAnalysis &state, const SchematicParams &params,
                            llvm::LoopInfo &LI, llvm::Loop *loopScope) {
    // Phase 2: BFS backward from seedEdge through disabled edges to build DAG
    std::deque<CFGEdge> toVisit = {seedEdge};
    std::set<CFGEdge> visited;
    std::map<CFGEdge, std::vector<CFGEdge>> dagAdj;
    dagAdj[seedEdge]; // ensure seed node exists

    while (!toVisit.empty()) {
        CFGEdge ckpt = toVisit.front();
        toVisit.pop_front();
        if (visited.count(ckpt))
            continue;
        visited.insert(ckpt);

        SchematicBlock *bb = ckpt.src;
        if (!bb)
            continue;
        if (loopScope && bb->getLLVMBlock() && !loopScope->contains(bb->getLLVMBlock()))
            continue;

        for (SchematicBlock *pred : bb->predecessors()) {
            CFGEdge childEdge{pred, bb};

            if (visited.count(childEdge))
                continue;
            // Reference: cfg_modification.py traverses only DISABLED checkpoints.
            if (!isDisabledCheckpoint(solution, childEdge))
                continue;
            // Skip back-edges
            if (auto *llvmBB = bb->getLLVMBlock()) {
                if (llvm::Loop *dstLoop = LI.getLoopFor(llvmBB)) {
                    if (dstLoop->getHeader() == llvmBB && pred->getLLVMBlock() &&
                        dstLoop->contains(pred->getLLVMBlock()))
                        continue;
                }
            }
            if (loopScope && pred->getLLVMBlock() && !loopScope->contains(pred->getLLVMBlock()))
                continue;
            dagAdj[childEdge]; // ensure node exists
            dagAdj[ckpt].push_back(childEdge);
            if (!visited.count(childEdge))
                toVisit.push_back(childEdge);
        }
    }

    // Phase 3: Kahn's algorithm for topological sort, then longest-path propagation
    std::map<CFGEdge, unsigned> inDeg;
    for (auto &[node, children] : dagAdj) {
        if (inDeg.find(node) == inDeg.end())
            inDeg[node] = 0;
        for (auto &child : children)
            inDeg[child]++;
    }
    std::deque<CFGEdge> topoQueue;
    for (auto &[node, deg] : inDeg)
        if (deg == 0)
            topoQueue.push_back(node);

    std::map<CFGEdge, double> maxEToLeave;
    maxEToLeave[seedEdge] = seedEToLeave;

    while (!topoQueue.empty()) {
        CFGEdge ckpt = topoQueue.front();
        topoQueue.pop_front();

        double eToLeave = maxEToLeave.count(ckpt) ? maxEToLeave[ckpt] : 0.0;
        maxEToLeave.erase(ckpt);

        SchematicBlock *bb = ckpt.src;
        if (bb) {
            // Add block execution cost
            double blockCost = getBlockExecEnergy(bb, solution, cfg, state, params);
            eToLeave += blockCost;

            // Update E_to_leave if larger
            if (eToLeave > solution.blockMeta[bb].E_to_leave)
                solution.blockMeta[bb].E_to_leave = eToLeave;

            // Propagate to children
            for (auto &childEdge : dagAdj[ckpt]) {
                if (maxEToLeave.find(childEdge) == maxEToLeave.end() ||
                    eToLeave > maxEToLeave[childEdge])
                    maxEToLeave[childEdge] = eToLeave;
            }
        }

        // Advance topological sort
        for (auto &child : dagAdj[ckpt]) {
            inDeg[child]--;
            if (inDeg[child] == 0)
                topoQueue.push_back(child);
        }
    }
}

void propagateEnergyLeft(const CFGEdge &seedEdge, double seedELeft, SchematicSolution &solution,
                         const CFGAnalysis &cfg, const SchematicStateAnalysis &state,
                         const SchematicParams &params, llvm::LoopInfo &LI, llvm::Loop *loopScope) {
    // Deliberate divergence from the reference (cfg_modification.py:256-316): the
    // reference walks forward greedily, popping the max-cost pending edge and
    // dropping any proposal to an already-visited edge. At a join whose costlier
    // incoming path has more hops, the join's outgoing edge can be popped via the
    // cheap path before the expensive frontier arrives, so blocks downstream of
    // the join get a best-case (too high) E_left. Mirror propagateEnergyToLeave
    // instead: build the DAG of reachable disabled edges first, then propagate
    // the worst-case (max) accumulated cost in topological order.

    // Phase 2: BFS forward from seedEdge through disabled edges to build DAG
    std::deque<CFGEdge> toVisit = {seedEdge};
    std::set<CFGEdge> visited;
    std::map<CFGEdge, std::vector<CFGEdge>> dagAdj;
    dagAdj[seedEdge]; // ensure seed node exists

    while (!toVisit.empty()) {
        CFGEdge ckpt = toVisit.front();
        toVisit.pop_front();
        if (!visited.insert(ckpt).second)
            continue;

        SchematicBlock *bb = ckpt.dst;
        if (!bb)
            continue;
        if (loopScope && bb->getLLVMBlock() && !loopScope->contains(bb->getLLVMBlock()))
            continue;

        for (SchematicBlock *succ : bb->successors()) {
            CFGEdge childEdge{bb, succ};

            if (visited.count(childEdge))
                continue;
            // Reference: cfg_modification.py traverses only DISABLED checkpoints.
            if (!isDisabledCheckpoint(solution, childEdge))
                continue;
            // Skip back-edges
            if (auto *llvmSucc = succ->getLLVMBlock()) {
                if (llvm::Loop *dstLoop = LI.getLoopFor(llvmSucc)) {
                    if (dstLoop->getHeader() == llvmSucc && bb->getLLVMBlock() &&
                        dstLoop->contains(bb->getLLVMBlock()))
                        continue;
                }
            }
            if (loopScope && succ->getLLVMBlock() && !loopScope->contains(succ->getLLVMBlock()))
                continue;
            dagAdj[childEdge]; // ensure node exists
            dagAdj[ckpt].push_back(childEdge);
            toVisit.push_back(childEdge);
        }
    }

    // Phase 3: Kahn's algorithm for topological sort, then longest-path propagation
    std::map<CFGEdge, unsigned> inDeg;
    for (auto &[node, children] : dagAdj) {
        if (inDeg.find(node) == inDeg.end())
            inDeg[node] = 0;
        for (auto &child : children)
            inDeg[child]++;
    }
    std::deque<CFGEdge> topoQueue;
    for (auto &[node, deg] : inDeg)
        if (deg == 0)
            topoQueue.push_back(node);

    std::map<CFGEdge, double> maxCost;
    maxCost[seedEdge] = 0.0;

    while (!topoQueue.empty()) {
        CFGEdge ckpt = topoQueue.front();
        topoQueue.pop_front();

        double cost = maxCost.count(ckpt) ? maxCost[ckpt] : 0.0;
        maxCost.erase(ckpt);

        SchematicBlock *bb = ckpt.dst;
        if (bb) {
            // Add block execution cost and compute energy_left
            cost += getBlockExecEnergy(bb, solution, cfg, state, params);
            double energyLeft = seedELeft - cost;

            // Update E_left if smaller
            if (energyLeft < solution.blockMeta[bb].E_left)
                solution.blockMeta[bb].E_left = energyLeft;

            // Propagate worst-case (max) accumulated cost to children
            for (auto &childEdge : dagAdj[ckpt]) {
                if (maxCost.find(childEdge) == maxCost.end() || cost > maxCost[childEdge])
                    maxCost[childEdge] = cost;
            }
        }

        // Advance topological sort
        for (auto &child : dagAdj[ckpt]) {
            inDeg[child]--;
            if (inDeg[child] == 0)
                topoQueue.push_back(child);
        }
    }
}

} // namespace checkpoint
