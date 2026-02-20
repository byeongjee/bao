#include "rockclimb/RockClimbOptimizer.h"
#include "common/BlockUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <algorithm>
#include <queue>

namespace checkpoint {

RockClimbOptimizer::RockClimbOptimizer(const CFGAnalysis &cfg,
                                       double E_safe,
                                       llvm::LoopInfo &LI,
                                       llvm::Function &F,
                                       EnergyEstimator *estimator)
    : cfg_(cfg), E_safe_(E_safe), LI_(LI), F_(F), estimator_(estimator) {
    // Build block name to BasicBlock* mapping
    for (llvm::BasicBlock &BB : F_) {
        std::string name = getBlockName(BB, F_);
        blockMap_[name] = &BB;
    }

    identifyLoopHeaders();
    identifyCallSiteBlocks();
    buildAdjacencyMaps();
    computeTopologicalOrder();
}

void RockClimbOptimizer::identifyLoopHeaders() {
    loopHeaders_.clear();
    for (llvm::Loop *L : LI_) {
        std::queue<llvm::Loop*> worklist;
        worklist.push(L);
        while (!worklist.empty()) {
            llvm::Loop *current = worklist.front();
            worklist.pop();

            llvm::BasicBlock *header = current->getHeader();
            if (header) {
                std::string name = getBlockName(*header, F_);
                loopHeaders_.insert(name);
            }

            for (llvm::Loop *subLoop : *current) {
                worklist.push(subLoop);
            }
        }
    }
}

void RockClimbOptimizer::identifyCallSiteBlocks() {
    callSiteBlocks_.clear();
    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            llvm::CallBase *CB = llvm::dyn_cast<llvm::CallBase>(&I);
            if (!CB) continue;

            // Skip intrinsics (dbg info, lifetime markers, etc.)
            if (llvm::isa<llvm::IntrinsicInst>(&I)) continue;

            // Skip inline assembly
            if (CB->isInlineAsm()) continue;

            llvm::Function *callee = CB->getCalledFunction();
            // Indirect calls are also call sites
            if (!callee || !callee->isIntrinsic()) {
                std::string name = getBlockName(BB, F_);
                callSiteBlocks_.insert(name);
                break;  // One call is enough to mark the block
            }
        }
    }
}

void RockClimbOptimizer::buildAdjacencyMaps() {
    successors_.clear();

    // Initialize empty lists for all blocks
    for (const auto &block : cfg_.getBlocks()) {
        successors_[block] = {};
    }

    for (const auto &edge : cfg_.getEdges()) {
        successors_[edge.first].push_back(edge.second);
    }
}

void RockClimbOptimizer::computeTopologicalOrder() {
    topoOrder_.clear();

    const auto &blocks = cfg_.getBlocks();
    if (blocks.empty()) return;

    std::string entry = cfg_.getEntryBlock();

    // Build in-degree map from edges
    std::map<std::string, int> inDegree;
    for (const auto &block : blocks) {
        inDegree[block] = 0;
    }
    for (const auto &edge : cfg_.getEdges()) {
        inDegree[edge.second]++;
    }

    // Kahn's algorithm with BFS from entry
    // Note: CFG may have cycles (loops), handle by allowing negative in-degree
    std::queue<std::string> queue;
    std::set<std::string> visited;

    queue.push(entry);

    while (!queue.empty()) {
        std::string curr = queue.front();
        queue.pop();

        if (visited.count(curr)) continue;
        visited.insert(curr);
        topoOrder_.push_back(curr);

        for (const auto &succ : successors_[curr]) {
            inDegree[succ]--;
            if (inDegree[succ] <= 0 && !visited.count(succ)) {
                queue.push(succ);
            }
        }
    }

    // Add any remaining unreachable blocks
    for (const auto &block : blocks) {
        if (!visited.count(block)) {
            topoOrder_.push_back(block);
        }
    }
}

double RockClimbOptimizer::getBlockCost(const std::string &block) const {
    double baseCost = cfg_.getBlockInfo(block).energyCost;
    auto it = extraBlockCosts_.find(block);
    if (it != extraBlockCosts_.end()) {
        baseCost += it->second;
    }
    return baseCost;
}

void RockClimbOptimizer::setExtraBlockCosts(
    const std::map<std::string, double> &costs) {
    extraBlockCosts_ = costs;
}

std::vector<std::string> RockClimbOptimizer::getInfeasibleBlocks() const {
    std::vector<std::string> infeasible;
    for (const auto &block : cfg_.getBlocks()) {
        double energy = getBlockCost(block);
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

    if (topoOrder_.empty()) {
        return result;
    }

    // Algorithm 1 from RockClimb paper: path-aware region partitioning
    //
    // Lines 1-4: Initialize IncomeCycle_bbi = 0 for all blocks
    std::map<std::string, double> IncomeCycle;
    for (const auto &block : topoOrder_) {
        IncomeCycle[block] = 0.0;
    }

    // Track which blocks are region boundaries
    std::set<std::string> boundarySet;
    // Entry block always starts a region
    boundarySet.insert(topoOrder_[0]);

    // Store accum_cycle per block for successor propagation
    std::map<std::string, double> accumCycleMap;

    // Lines 5-16: Process blocks in topological order
    for (size_t i = 0; i < topoOrder_.size(); ++i) {
        const std::string &block = topoOrder_[i];
        double Cycle_bbi = getBlockCost(block);

        // Check mandatory boundaries: loop headers and function call sites
        bool isMandatoryBoundary = false;
        if (block != topoOrder_[0]) {  // Entry already handled
            if (loopHeaders_.count(block) || callSiteBlocks_.count(block)) {
                isMandatoryBoundary = true;
                boundarySet.insert(block);
            }
        }

        bool isBoundary = boundarySet.count(block) > 0;

        double accum_cycle;
        // Lines 6-10: Compute accum_cycle
        if (isBoundary) {
            // Line 7: Region start — accum resets to just this block's cost
            accum_cycle = Cycle_bbi;
        } else {
            // Line 9: accum_cycle = Cycle_bbi + IncomeCycle_bbi
            accum_cycle = Cycle_bbi + IncomeCycle[block];
        }

        // Lines 11-15: While accum_cycle > threshold, place boundary / split
        if (accum_cycle > E_safe_ && !isBoundary) {
            // Place boundary at this block
            boundarySet.insert(block);
            // Reset: this block starts a new region
            accum_cycle = Cycle_bbi;
        }

        // After boundary placement, if single block still exceeds threshold,
        // try splitting (Algorithm 1 while loop, lines 11-15)
        while (accum_cycle > E_safe_ && estimator_) {
            std::string newBlock = splitBlock(block, E_safe_, i);
            if (newBlock.empty()) {
                // Can't split further — mark infeasible
                result.feasible = false;
                result.errorMessage = "Block '" + block +
                    "' exceeds E_safe and cannot be split further";
                return result;
            }

            // After splitting:
            // - 'block' (topoOrder_[i]) is now the first half (smaller)
            // - 'newBlock' is inserted at topoOrder_[i+1] (second half)
            // Recalculate cost for first half (current block)
            Cycle_bbi = getBlockCost(block);
            accum_cycle = Cycle_bbi;
            // The while loop will re-check; the second half will be processed
            // in the next iteration of the outer for loop
        }

        // Store accum_cycle for propagation
        accumCycleMap[block] = accum_cycle;

        // Propagate to successors: IncomeCycle_succ = max(IncomeCycle_succ, accum_cycle)
        auto succIt = successors_.find(block);
        if (succIt != successors_.end()) {
            for (const auto &succ : succIt->second) {
                if (IncomeCycle.find(succ) == IncomeCycle.end()) {
                    IncomeCycle[succ] = 0.0;
                }
                IncomeCycle[succ] = std::max(IncomeCycle[succ], accum_cycle);
            }
        }
    }

    // Build result: collect boundaries in topological order and form regions
    result.regionBoundaries.clear();
    result.regions.clear();

    std::string currentRegionStart;
    std::vector<std::string> currentRegionBlocks;
    double currentRegionEnergy = 0.0;

    for (const auto &block : topoOrder_) {
        if (boundarySet.count(block)) {
            // Finish previous region (if any)
            if (!currentRegionBlocks.empty()) {
                RegionInfo region;
                region.startBlock = currentRegionStart;
                region.blocks = currentRegionBlocks;
                region.totalEnergy = currentRegionEnergy;
                result.regions.push_back(region);
            }

            // Start new region
            result.regionBoundaries.push_back(block);
            currentRegionStart = block;
            currentRegionBlocks.clear();
            currentRegionEnergy = 0.0;
        }

        currentRegionBlocks.push_back(block);
        currentRegionEnergy += getBlockCost(block);
    }

    // Finish last region
    if (!currentRegionBlocks.empty()) {
        RegionInfo region;
        region.startBlock = currentRegionStart;
        region.blocks = currentRegionBlocks;
        region.totalEnergy = currentRegionEnergy;
        result.regions.push_back(region);
    }

    return result;
}

std::string RockClimbOptimizer::splitBlock(const std::string &blockName,
                                            double threshold,
                                            size_t insertIdx) {
    if (!estimator_) return "";

    auto it = blockMap_.find(blockName);
    if (it == blockMap_.end()) return "";

    llvm::BasicBlock *BB = it->second;

    // Find the split point: accumulate per-instruction energy until threshold
    double accumulated = 0.0;
    llvm::Instruction *splitPoint = nullptr;

    for (llvm::Instruction &I : *BB) {
        // Don't split before a PHI node or landingpad
        if (llvm::isa<llvm::PHINode>(&I) || llvm::isa<llvm::LandingPadInst>(&I))
            continue;

        double instCost = estimator_->getInstructionCost(I);

        if (accumulated + instCost > threshold && splitPoint) {
            // Split before this instruction
            break;
        }

        accumulated += instCost;
        splitPoint = &I;

        // Don't set split point to the terminator
        if (I.isTerminator()) {
            splitPoint = nullptr;
        }
    }

    if (!splitPoint) return "";  // Can't split (block too small or all PHIs)

    // Split after splitPoint — the next instruction becomes the start of the new block
    llvm::Instruction *splitBefore = splitPoint->getNextNode();
    if (!splitBefore || splitBefore->isTerminator()) {
        // Nothing meaningful to split off
        // If the terminator is the only thing left, try splitting before splitPoint
        if (splitPoint->isTerminator()) return "";
        splitBefore = splitPoint;
    }

    // Perform the split
    llvm::BasicBlock *newBB = BB->splitBasicBlock(splitBefore,
                                                   blockName + ".split");

    // Update local data structures
    std::string newName = getBlockName(*newBB, F_);
    blockMap_[newName] = newBB;

    // Update blockMap for original block (pointer unchanged but name mapping still valid)
    blockMap_[blockName] = BB;

    // Update adjacency: old successors of blockName now belong to newName
    // blockName's only successor is now newName (due to splitBasicBlock adding br)
    auto oldSuccessors = successors_[blockName];
    successors_[blockName] = {newName};
    successors_[newName] = oldSuccessors;

    // Insert new block into topological order right after current block
    topoOrder_.insert(topoOrder_.begin() + static_cast<long>(insertIdx) + 1,
                      newName);

    // The new block inherits boundary status from original if it had call sites
    // Re-check for call sites in both halves
    bool origHasCall = false, newHasCall = false;
    for (llvm::Instruction &I : *BB) {
        if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (!llvm::isa<llvm::IntrinsicInst>(&I) && !CB->isInlineAsm()) {
                llvm::Function *callee = CB->getCalledFunction();
                if (!callee || !callee->isIntrinsic()) {
                    origHasCall = true;
                    break;
                }
            }
        }
    }
    for (llvm::Instruction &I : *newBB) {
        if (auto *CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (!llvm::isa<llvm::IntrinsicInst>(&I) && !CB->isInlineAsm()) {
                llvm::Function *callee = CB->getCalledFunction();
                if (!callee || !callee->isIntrinsic()) {
                    newHasCall = true;
                    break;
                }
            }
        }
    }

    // Update call site blocks
    if (!origHasCall) callSiteBlocks_.erase(blockName);
    if (newHasCall) callSiteBlocks_.insert(newName);

    // Recalculate energy costs for both halves using estimator
    // Store as extra costs that override the original CFG cost
    double origNewCost = estimator_->estimate(*BB).cost;
    double splitNewCost = estimator_->estimate(*newBB).cost;

    // The original CFG still has the old cost for blockName.
    // We use extraBlockCosts_ to adjust: effective cost = cfg_cost + extra
    // We want effective cost = origNewCost + any existing extra
    // So extra = origNewCost - cfg_original_cost
    double cfgOrigCost = cfg_.getBlockInfo(blockName).energyCost;
    extraBlockCosts_[blockName] = origNewCost - cfgOrigCost;

    // For the new block, cfg_ doesn't know about it, so getBlockInfo would fail.
    // We store the full cost as extra, and getBlockCost will handle it.
    // But getBlockCost calls cfg_.getBlockInfo which will fail for new blocks.
    // We need to handle this case.
    extraBlockCosts_[newName] = splitNewCost;

    return newName;
}

} // namespace checkpoint
