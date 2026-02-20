#include "rockclimb/RockClimbOptimizer.h"
#include "common/BlockUtils.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include <algorithm>
#include <queue>

namespace checkpoint {
namespace {

static bool isCallSite(const llvm::Instruction &I) {
    const auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
    if (!CB) {
        return false;
    }
    if (llvm::isa<llvm::IntrinsicInst>(&I) || CB->isInlineAsm()) {
        return false;
    }

    llvm::Function *callee = CB->getCalledFunction();
    return !callee || !callee->isIntrinsic();
}

static bool blockHasCallSite(const llvm::BasicBlock &BB) {
    for (const llvm::Instruction &I : BB) {
        if (isCallSite(I)) {
            return true;
        }
    }
    return false;
}

} // namespace

llvm::BasicBlock *RockClimbOptimizer::resolveBlock(
    const llvm::WeakTrackingVH &handle) {
    auto *BB = llvm::cast_or_null<llvm::BasicBlock>(handle);
    assert(BB && "WeakTrackingVH resolved to null — block was deleted");
    return BB;
}

RockClimbOptimizer::RockClimbOptimizer(const CFGAnalysis &cfg,
                                       double E_safe,
                                       llvm::LoopInfo &LI,
                                       llvm::Function &F,
                                       EnergyEstimator *estimator)
    : cfg_(cfg), E_safe_(E_safe), LI_(LI), F_(F), estimator_(estimator) {
    // Build energyCosts_ from CFGAnalysis
    for (llvm::BasicBlock &BB : F_) {
        energyCosts_[&BB] = cfg_.getBlockInfo(&BB).energyCost;
    }

    identifyLoopHeaders();
    identifyCallSiteBlocks();
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
                loopHeaders_.insert(header);
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
        if (blockHasCallSite(BB)) {
            callSiteBlocks_.insert(&BB);
        }
    }
}

void RockClimbOptimizer::computeTopologicalOrder() {
    using namespace llvm;
    topoOrder_.clear();

    if (F_.empty()) return;

    // Reverse post-order guarantees all predecessors (modulo back-edges)
    // are visited before each block. Back-edges are safe to ignore because
    // loop headers are forced boundaries that reset energy accumulation.
    SmallPtrSet<BasicBlock*, 32> visited;
    ReversePostOrderTraversal<Function*> RPOT(&F_);
    for (BasicBlock *BB : RPOT) {
        topoOrder_.push_back(WeakTrackingVH(BB));
        visited.insert(BB);
    }

    // Add any unreachable blocks not covered by RPO
    for (BasicBlock &BB : F_) {
        if (!visited.count(&BB)) {
            topoOrder_.push_back(WeakTrackingVH(&BB));
        }
    }
}

double RockClimbOptimizer::getBlockCost(llvm::BasicBlock *BB) const {
    auto it = energyCosts_.find(BB);
    if (it != energyCosts_.end()) {
        return it->second;
    }
    return 0.0;
}

void RockClimbOptimizer::setExtraBlockCosts(
    const llvm::DenseMap<llvm::BasicBlock*, double> &costs) {
    for (const auto &entry : costs) {
        energyCosts_[entry.first] += entry.second;
    }
}

std::vector<llvm::BasicBlock*> RockClimbOptimizer::getInfeasibleBlocks() const {
    std::vector<llvm::BasicBlock*> infeasible;
    for (llvm::BasicBlock &BB : F_) {
        double energy = getBlockCost(&BB);
        if (energy >= E_safe_) {
            infeasible.push_back(&BB);
        }
    }
    return infeasible;
}

RockClimbOptimizer::Result RockClimbOptimizer::optimize() {
    return partitionRegions();
}

RockClimbOptimizer::Result RockClimbOptimizer::partitionRegions() {
    using namespace llvm;
    Result result;
    result.feasible = true;

    if (topoOrder_.empty()) {
        return result;
    }

    // Algorithm 1 from RockClimb paper: path-aware region partitioning
    //
    // Lines 1-4: Initialize IncomeCycle_bbi = 0 for all blocks
    DenseMap<BasicBlock*, double> incomeCycle;
    for (const auto &handle : topoOrder_) {
        incomeCycle[resolveBlock(handle)] = 0.0;
    }

    // Track which blocks are region boundaries
    SmallPtrSet<BasicBlock*, 16> boundarySet;
    // Entry block always starts a region
    BasicBlock *entryBB = resolveBlock(topoOrder_[0]);
    boundarySet.insert(entryBB);

    // Lines 5-16: Process blocks in topological order
    for (size_t i = 0; i < topoOrder_.size(); ++i) {
        BasicBlock *BB = resolveBlock(topoOrder_[i]);
        double Cycle_bbi = getBlockCost(BB);

        // Check mandatory boundaries: loop headers and function call sites
        if (BB != entryBB) {  // Entry already handled
            if (loopHeaders_.count(BB) || callSiteBlocks_.count(BB)) {
                boundarySet.insert(BB);
            }
        }

        bool isBoundary = boundarySet.count(BB) > 0;

        double accum_cycle;
        // Lines 6-10: Compute accum_cycle
        if (isBoundary) {
            // Line 7: Region start — accum resets to just this block's cost
            accum_cycle = Cycle_bbi;
        } else {
            // Line 9: accum_cycle = Cycle_bbi + IncomeCycle_bbi
            accum_cycle = Cycle_bbi + incomeCycle[BB];
        }

        // Lines 11-15: if accum_cycle >= threshold, place boundary / split
        if (accum_cycle >= E_safe_ && !isBoundary) {
            // Place boundary at this block
            boundarySet.insert(BB);
            // Reset: this block starts a new region
            accum_cycle = Cycle_bbi;
        }

        // After boundary placement, if single block still exceeds threshold,
        // try splitting (Algorithm 1 while loop, lines 11-15)
        while (accum_cycle >= E_safe_ && estimator_) {
            BasicBlock *newBB = splitBlock(BB, E_safe_, i);
            if (!newBB) {
                // Can't split further — mark infeasible
                result.feasible = false;
                result.errorMessage = "Block '" +
                    getBlockName(*BB, F_) +
                    "' exceeds E_safe and cannot be split further";
                return result;
            }

            // After splitting:
            // - BB (topoOrder_[i]) is now the first half (smaller)
            // - newBB is inserted at topoOrder_[i+1] (second half)
            // Recalculate cost for first half (current block)
            Cycle_bbi = getBlockCost(BB);
            accum_cycle = Cycle_bbi;
            // The while loop will re-check; the second half will be processed
            // in the next iteration of the outer for loop
        }

        // Propagate to successors: IncomeCycle_succ = max(IncomeCycle_succ, accum_cycle)
        for (BasicBlock *succ : successors(BB)) {
            if (incomeCycle.find(succ) == incomeCycle.end()) {
                incomeCycle[succ] = 0.0;
            }
            incomeCycle[succ] = std::max(incomeCycle[succ], accum_cycle);
        }
    }

    // Build result: collect boundaries in topological order and form regions
    result.regionBoundaries.clear();
    result.regions.clear();

    WeakTrackingVH currentRegionStart;
    std::vector<WeakTrackingVH> currentRegionBlocks;
    double currentRegionEnergy = 0.0;

    for (const auto &handle : topoOrder_) {
        BasicBlock *BB = resolveBlock(handle);
        if (boundarySet.count(BB)) {
            // Finish previous region (if any)
            if (!currentRegionBlocks.empty()) {
                RegionInfo region;
                region.startBlock = currentRegionStart;
                region.blocks = currentRegionBlocks;
                region.totalEnergy = currentRegionEnergy;
                result.regions.push_back(region);
            }

            // Start new region
            result.regionBoundaries.push_back(handle);
            currentRegionStart = handle;
            currentRegionBlocks.clear();
            currentRegionEnergy = 0.0;
        }

        currentRegionBlocks.push_back(handle);
        currentRegionEnergy += getBlockCost(BB);
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

llvm::BasicBlock *RockClimbOptimizer::splitBlock(llvm::BasicBlock *BB,
                                                  double threshold,
                                                  size_t insertIdx) {
    if (!estimator_) return nullptr;
    if (!BB) return nullptr;

    // Find the split point: accumulate per-instruction energy until threshold
    double accumulated = 0.0;
    llvm::Instruction *splitPoint = nullptr;

    for (llvm::Instruction &I : *BB) {
        // Don't split before a PHI node or landingpad
        if (llvm::isa<llvm::PHINode>(&I) || llvm::isa<llvm::LandingPadInst>(&I))
            continue;

        double instCost = estimator_->getInstructionCost(I);

        if (accumulated + instCost >= threshold && splitPoint) {
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

    if (!splitPoint) return nullptr;  // Can't split (block too small or all PHIs)

    // Split after splitPoint — the next instruction becomes the start of the new block
    llvm::Instruction *splitBefore = splitPoint->getNextNode();
    if (!splitBefore || splitBefore->isTerminator()) {
        // Nothing meaningful to split off
        // If the terminator is the only thing left, try splitting before splitPoint
        if (splitPoint->isTerminator()) return nullptr;
        splitBefore = splitPoint;
    }

    // Perform the split
    std::string blockName = getBlockName(*BB, F_);
    llvm::BasicBlock *newBB = BB->splitBasicBlock(splitBefore,
                                                   blockName + ".split");

    // Update energyCosts_ for both halves directly
    energyCosts_[BB] = estimator_->estimate(*BB).cost;
    energyCosts_[newBB] = estimator_->estimate(*newBB).cost;

    // Insert new block into topological order right after current block
    topoOrder_.insert(topoOrder_.begin() + static_cast<long>(insertIdx) + 1,
                      llvm::WeakTrackingVH(newBB));

    // Re-check for call sites in both halves
    bool origHasCall = blockHasCallSite(*BB);
    bool newHasCall = blockHasCallSite(*newBB);

    // Update call site blocks
    if (!origHasCall) callSiteBlocks_.erase(BB);
    if (newHasCall) callSiteBlocks_.insert(newBB);

    return newBB;
}

} // namespace checkpoint
