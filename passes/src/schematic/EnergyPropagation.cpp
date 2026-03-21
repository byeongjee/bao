// passes/src/schematic/EnergyPropagation.cpp
#include "schematic/EnergyPropagation.h"

#include <algorithm>
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
    // Phase 1: Initialize seenLoops, pre-adding the seed block's loop
    std::set<llvm::BasicBlock *> seenLoops;
    SchematicBlock *bbBefore = seedEdge.src;
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

        SchematicBlock *bb = ckpt.src;
        if (!bb)
            continue;
        if (loopScope && bb->getLLVMBlock() && !loopScope->contains(bb->getLLVMBlock()))
            continue;

        for (SchematicBlock *pred : bb->predecessors()) {
            CFGEdge childEdge{pred, bb};

            if (visited.count(childEdge))
                continue;
            // Skip enabled checkpoints (only traverse DISABLED edges)
            if (solution.enabledCheckpoints.count(childEdge))
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
            // Inner-loop scaling — skip for nested inner loops when propagating
            // within an outer loopScope. Inner loop energy is managed by its own
            // checkpoint mechanism; re-applying seenLoops would double-count.
            auto metaIt = solution.blockMeta.find(bb);
            if (metaIt != solution.blockMeta.end() && metaIt->second.loop.has_value()) {
                llvm::Loop *markLoop = metaIt->second.loop->loop;
                if (!loopScope || markLoop == loopScope) {
                    llvm::BasicBlock *loopHeader = markLoop->getHeader();
                    if (!seenLoops.count(loopHeader)) {
                        seenLoops.insert(loopHeader);
                        eToLeave +=
                            (metaIt->second.loop->nbIter - 1) * metaIt->second.loop->costOneIt;
                    }
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
    SchematicBlock *bbAfter = seedEdge.dst;
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

        SchematicBlock *bb = ckpt.dst;
        if (!bb)
            continue;
        if (loopScope && bb->getLLVMBlock() && !loopScope->contains(bb->getLLVMBlock()))
            continue;

        // Inner-loop scaling (reference lines 293-295) — skip for nested inner
        // loops when propagating within an outer loopScope.
        auto metaIt = solution.blockMeta.find(bb);
        if (metaIt != solution.blockMeta.end() && metaIt->second.loop.has_value()) {
            llvm::Loop *markLoop = metaIt->second.loop->loop;
            if (!loopScope || markLoop == loopScope) {
                llvm::BasicBlock *loopHeader = markLoop->getHeader();
                if (!seenLoops.count(loopHeader)) {
                    seenLoops.insert(loopHeader);
                    cost += (metaIt->second.loop->nbIter - 1) * metaIt->second.loop->costOneIt;
                }
            }
        }

        // Add block cost and compute energy_left (reference lines 298-302)
        double blockCost = getBlockExecEnergy(bb, solution, cfg, state, params);
        cost += blockCost;
        double energyLeft = seedELeft - cost;
        if (energyLeft < solution.blockMeta[bb].E_left)
            solution.blockMeta[bb].E_left = energyLeft;

        // Forward to successor disabled edges (reference lines 310-316)
        for (SchematicBlock *succ : bb->successors()) {
            CFGEdge childEdge{bb, succ};

            if (visited.count(childEdge))
                continue;
            if (solution.enabledCheckpoints.count(childEdge))
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

            if (ckptCost.find(childEdge) == ckptCost.end() || cost < ckptCost[childEdge])
                ckptCost[childEdge] = cost;
        }
    }
}

} // namespace checkpoint
