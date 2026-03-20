// passes/src/schematic/EnergyPropagation.cpp
#include "schematic/EnergyPropagation.h"

#include "llvm/IR/CFG.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace checkpoint {

double getBlockExecEnergy(llvm::BasicBlock *BB, const SchematicSolution &solution,
                          const CFGAnalysis &cfg, const SchematicStateAnalysis &state,
                          const SchematicParams &params) {
    double E = cfg.getBlockInfo(BB).energyCost;
    auto allocIt = solution.decidedPlacements.find(BB);
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
    // Phase 1: Initialize seenLoops, pre-adding the seed block's loop
    std::set<llvm::BasicBlock *> seenLoops;
    llvm::BasicBlock *bbBefore = seedEdge.src;
    if (bbBefore) {
        auto metaIt = solution.blockMeta.find(bbBefore);
        if (metaIt != solution.blockMeta.end() && metaIt->second.loop.has_value())
            seenLoops.insert(metaIt->second.loop->loop->getHeader());
    }

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

        llvm::BasicBlock *bb = ckpt.src;
        if (!bb)
            continue;
        if (loopScope && !loopScope->contains(bb))
            continue;

        for (llvm::BasicBlock *pred : llvm::predecessors(bb)) {
            CFGEdge childEdge{pred, bb};

            if (visited.count(childEdge))
                continue;
            // Skip enabled checkpoints (only traverse DISABLED edges)
            if (solution.enabledCheckpoints.count(childEdge))
                continue;
            // Skip back-edges
            if (llvm::Loop *dstLoop = LI.getLoopFor(bb))
                if (dstLoop->getHeader() == bb && dstLoop->contains(pred))
                    continue;
            if (loopScope && !loopScope->contains(pred))
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

        llvm::BasicBlock *bb = ckpt.src;
        if (bb) {
            // Inner-loop scaling
            auto metaIt = solution.blockMeta.find(bb);
            if (metaIt != solution.blockMeta.end() && metaIt->second.loop.has_value()) {
                llvm::BasicBlock *loopHeader = metaIt->second.loop->loop->getHeader();
                if (!seenLoops.count(loopHeader)) {
                    seenLoops.insert(loopHeader);
                    eToLeave += (metaIt->second.loop->nbIter - 1) * metaIt->second.loop->costOneIt;
                }
            }

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
    // Phase 1: Initialize seenLoops (reference lines 271-277)
    std::set<llvm::BasicBlock *> seenLoops;
    llvm::BasicBlock *bbAfter = seedEdge.dst;
    if (bbAfter) {
        auto metaIt = solution.blockMeta.find(bbAfter);
        if (metaIt != solution.blockMeta.end() && metaIt->second.loop.has_value())
            seenLoops.insert(metaIt->second.loop->loop->getHeader());
    }

    // Priority queue: edge -> accumulated cost (reference: chkpt_cost)
    std::map<CFGEdge, double> ckptCost;
    ckptCost[seedEdge] = 0.0;
    std::set<CFGEdge> visited;

    // Phase 2: Priority queue loop -- max cost first (reference lines 279-316)
    while (!ckptCost.empty()) {
        // Select checkpoint with highest accumulated cost (reference line 281)
        auto maxIt =
            std::max_element(ckptCost.begin(), ckptCost.end(),
                             [](const auto &a, const auto &b) { return a.second < b.second; });
        CFGEdge ckpt = maxIt->first;
        double cost = maxIt->second;

        visited.insert(ckpt);
        ckptCost.erase(maxIt);

        llvm::BasicBlock *bb = ckpt.dst;
        if (!bb)
            continue;
        if (loopScope && !loopScope->contains(bb))
            continue;

        // Inner-loop scaling (reference lines 293-295)
        auto metaIt = solution.blockMeta.find(bb);
        if (metaIt != solution.blockMeta.end() && metaIt->second.loop.has_value()) {
            llvm::BasicBlock *loopHeader = metaIt->second.loop->loop->getHeader();
            if (!seenLoops.count(loopHeader)) {
                seenLoops.insert(loopHeader);
                cost += (metaIt->second.loop->nbIter - 1) * metaIt->second.loop->costOneIt;
            }
        }

        // Add block cost and compute energy_left (reference lines 298-302)
        double blockCost = getBlockExecEnergy(bb, solution, cfg, state, params);
        cost += blockCost;
        double energyLeft = seedELeft - cost;
        if (energyLeft < solution.blockMeta[bb].E_left)
            solution.blockMeta[bb].E_left = energyLeft;

        // Forward to successor disabled edges (reference lines 310-316)
        for (llvm::BasicBlock *succ : llvm::successors(bb)) {
            CFGEdge childEdge{bb, succ};

            if (visited.count(childEdge))
                continue;
            if (solution.enabledCheckpoints.count(childEdge))
                continue;
            // Skip back-edges
            if (llvm::Loop *dstLoop = LI.getLoopFor(succ))
                if (dstLoop->getHeader() == succ && dstLoop->contains(bb))
                    continue;
            if (loopScope && !loopScope->contains(succ))
                continue;

            if (ckptCost.find(childEdge) == ckptCost.end() || cost < ckptCost[childEdge])
                ckptCost[childEdge] = cost;
        }
    }
}

} // namespace checkpoint
