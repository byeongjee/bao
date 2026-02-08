#include "rockclimb/RockClimbOptimizer.h"
#include "common/BlockUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/IR/CFG.h"

#include <algorithm>
#include <queue>

namespace checkpoint {

RockClimbOptimizer::RockClimbOptimizer(const CFGAnalysis &cfg,
                                       double E_safe,
                                       llvm::LoopInfo &LI)
    : cfg_(cfg), E_safe_(E_safe), LI_(LI) {
    // Build block name to BasicBlock* mapping by iterating the function
    // We need to recover the Function from LoopInfo
    if (!LI_.empty()) {
        llvm::Function *F = (*LI_.begin())->getHeader()->getParent();
        for (llvm::BasicBlock &BB : *F) {
            std::string name = getBlockName(BB, *F);
            blockMap_[name] = &BB;
        }
    }

    identifyLoopHeaders();
    computeTopologicalOrder();
}

void RockClimbOptimizer::identifyLoopHeaders() {
    loopHeaders_.clear();
    for (llvm::Loop *L : LI_) {
        // Process all loops including nested ones
        std::queue<llvm::Loop*> worklist;
        worklist.push(L);
        while (!worklist.empty()) {
            llvm::Loop *current = worklist.front();
            worklist.pop();

            llvm::BasicBlock *header = current->getHeader();
            if (header) {
                llvm::Function *F = header->getParent();
                std::string name = getBlockName(*header, *F);
                loopHeaders_.insert(name);
            }

            // Add nested loops
            for (llvm::Loop *subLoop : *current) {
                worklist.push(subLoop);
            }
        }
    }
}

void RockClimbOptimizer::computeTopologicalOrder() {
    topoOrder_.clear();

    // Get blocks in reverse post-order (approximates topological order for CFG)
    // For a proper topological order in presence of loops, we use RPO
    // which visits predecessors before successors where possible

    const auto &blocks = cfg_.getBlocks();
    if (blocks.empty()) return;

    // Use the CFG's block order as a starting point
    // Then do BFS from entry to get a reasonable topological approximation
    std::string entry = cfg_.getEntryBlock();

    // Build adjacency list from edges
    std::map<std::string, std::vector<std::string>> adj;
    std::map<std::string, int> inDegree;
    for (const auto &block : blocks) {
        adj[block] = {};
        inDegree[block] = 0;
    }
    for (const auto &edge : cfg_.getEdges()) {
        adj[edge.first].push_back(edge.second);
        inDegree[edge.second]++;
    }

    // Kahn's algorithm for topological sort
    // Note: CFG may have cycles (loops), so we handle them specially
    std::queue<std::string> queue;
    std::set<std::string> visited;

    // Start with entry block (may have in-edges from back edges)
    queue.push(entry);

    while (!queue.empty()) {
        std::string curr = queue.front();
        queue.pop();

        if (visited.count(curr)) continue;
        visited.insert(curr);
        topoOrder_.push_back(curr);

        for (const auto &succ : adj[curr]) {
            inDegree[succ]--;
            if (inDegree[succ] <= 0 && !visited.count(succ)) {
                queue.push(succ);
            }
        }
    }

    // Add any remaining blocks not reachable in BFS order
    // (shouldn't happen for well-formed CFG but handle anyway)
    for (const auto &block : blocks) {
        if (!visited.count(block)) {
            topoOrder_.push_back(block);
        }
    }
}

std::vector<std::string> RockClimbOptimizer::getInfeasibleBlocks() const {
    std::vector<std::string> infeasible;
    for (const auto &block : cfg_.getBlocks()) {
        double energy = cfg_.getBlockInfo(block).energyCost;
        if (energy > E_safe_) {
            infeasible.push_back(block);
        }
    }
    return infeasible;
}

RockClimbOptimizer::Result RockClimbOptimizer::optimize() {
    return partitionRegions();
}

RockClimbOptimizer::Result RockClimbOptimizer::partitionRegions() {
    Result result;
    result.feasible = true;

    // Check for infeasible blocks first
    auto infeasible = getInfeasibleBlocks();
    if (!infeasible.empty()) {
        result.feasible = false;
        result.errorMessage = "Blocks exceed E_safe: ";
        for (size_t i = 0; i < infeasible.size(); ++i) {
            if (i > 0) result.errorMessage += ", ";
            result.errorMessage += infeasible[i];
        }
        return result;
    }

    if (topoOrder_.empty()) {
        result.feasible = true;
        return result;
    }

    // Algorithm 1 from RockClimb paper:
    // Greedy region partitioning with energy accumulation

    std::string currentRegionStart = topoOrder_[0];
    double accumulatedEnergy = 0.0;
    std::vector<std::string> currentRegionBlocks;

    // Entry block always starts a region
    result.regionBoundaries.push_back(currentRegionStart);

    for (const auto &block : topoOrder_) {
        double blockEnergy = cfg_.getBlockInfo(block).energyCost;
        bool isBoundary = false;

        // Mandatory boundary at loop headers
        if (loopHeaders_.count(block) && block != currentRegionStart) {
            isBoundary = true;
        }

        // Energy threshold boundary
        if (accumulatedEnergy + blockEnergy > E_safe_ && block != currentRegionStart) {
            isBoundary = true;
        }

        if (isBoundary) {
            // Finish current region
            RegionInfo region;
            region.startBlock = currentRegionStart;
            region.blocks = currentRegionBlocks;
            region.totalEnergy = accumulatedEnergy;
            result.regions.push_back(region);

            // Start new region
            currentRegionStart = block;
            currentRegionBlocks.clear();
            accumulatedEnergy = 0.0;
            result.regionBoundaries.push_back(block);
        }

        currentRegionBlocks.push_back(block);
        accumulatedEnergy += blockEnergy;
    }

    // Finish last region
    if (!currentRegionBlocks.empty()) {
        RegionInfo region;
        region.startBlock = currentRegionStart;
        region.blocks = currentRegionBlocks;
        region.totalEnergy = accumulatedEnergy;
        result.regions.push_back(region);
    }

    return result;
}

} // namespace checkpoint
