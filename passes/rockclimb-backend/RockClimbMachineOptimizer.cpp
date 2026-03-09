#include "RockClimbMachineOptimizer.h"
#include "MachineEnergyEstimator.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <queue>

using namespace llvm;

namespace checkpoint {
namespace {

/// Check if a MachineInstr is a real function call (not pseudo or inline asm)
static bool isMachineCallSite(const MachineInstr &MI) {
    if (!MI.isCall())
        return false;
    // Skip inline asm calls
    if (MI.isInlineAsm())
        return false;
    return true;
}

static bool blockHasMachineCallSite(const MachineBasicBlock &MBB) {
    for (const MachineInstr &MI : MBB) {
        if (isMachineCallSite(MI))
            return true;
    }
    return false;
}

} // namespace

RockClimbMachineOptimizer::RockClimbMachineOptimizer(MachineFunction &MF, MachineLoopInfo &MLI,
                                                     const MachineEnergyEstimator &estimator,
                                                     double E_safe)
    : MF_(MF), MLI_(MLI), estimator_(estimator), E_safe_(E_safe) {
    // Compute per-block energy costs
    for (MachineBasicBlock &MBB : MF_) {
        energyCosts_[&MBB] = estimator_.estimateBlock(MBB);
    }

    identifyLoopHeaders();
    identifyCallSiteBlocks();
    computeTopologicalOrder();
}

void RockClimbMachineOptimizer::identifyLoopHeaders() {
    loopHeaders_.clear();
    for (MachineLoop *L : MLI_) {
        std::queue<MachineLoop *> worklist;
        worklist.push(L);
        while (!worklist.empty()) {
            MachineLoop *current = worklist.front();
            worklist.pop();

            MachineBasicBlock *header = current->getHeader();
            if (header)
                loopHeaders_.insert(header);

            for (MachineLoop *subLoop : *current)
                worklist.push(subLoop);
        }
    }
}

void RockClimbMachineOptimizer::identifyCallSiteBlocks() {
    callSiteBlocks_.clear();
    for (MachineBasicBlock &MBB : MF_) {
        if (blockHasMachineCallSite(MBB))
            callSiteBlocks_.insert(&MBB);
    }
}

void RockClimbMachineOptimizer::computeTopologicalOrder() {
    topoOrder_.clear();
    if (MF_.empty())
        return;

    // Reverse post-order: all predecessors (modulo back-edges) visited first.
    // Back-edges are safe to ignore because loop headers are forced boundaries.
    SmallPtrSet<MachineBasicBlock *, 32> visited;
    ReversePostOrderTraversal<MachineFunction *> RPOT(&MF_);
    for (MachineBasicBlock *MBB : RPOT) {
        topoOrder_.push_back(MBB);
        visited.insert(MBB);
    }

    // Add any unreachable blocks not covered by RPO
    for (MachineBasicBlock &MBB : MF_) {
        if (!visited.count(&MBB))
            topoOrder_.push_back(&MBB);
    }
}

double RockClimbMachineOptimizer::getBlockCost(MachineBasicBlock *MBB) const {
    auto it = energyCosts_.find(MBB);
    return (it != energyCosts_.end()) ? it->second : 0.0;
}

void RockClimbMachineOptimizer::setExtraBlockCosts(
    const DenseMap<MachineBasicBlock *, double> &costs) {
    for (const auto &entry : costs)
        energyCosts_[entry.first] += entry.second;
}

std::vector<MachineBasicBlock *> RockClimbMachineOptimizer::getInfeasibleBlocks() const {
    std::vector<MachineBasicBlock *> infeasible;
    for (MachineBasicBlock &MBB : MF_) {
        if (getBlockCost(&MBB) >= E_safe_)
            infeasible.push_back(&MBB);
    }
    return infeasible;
}

MachineRockClimbResult RockClimbMachineOptimizer::optimize() {
    return partitionRegions();
}

MachineRockClimbResult RockClimbMachineOptimizer::partitionRegions() {
    MachineRockClimbResult result;
    result.feasible = true;

    if (topoOrder_.empty())
        return result;

    // Algorithm 1: Initialize IncomeCycle = 0 for all blocks
    DenseMap<MachineBasicBlock *, double> incomeCycle;
    for (MachineBasicBlock *MBB : topoOrder_)
        incomeCycle[MBB] = 0.0;

    // Track which blocks are region boundaries
    SmallPtrSet<MachineBasicBlock *, 16> boundarySet;
    MachineBasicBlock *entryMBB = topoOrder_[0];
    boundarySet.insert(entryMBB);

    // Process blocks in topological order
    for (size_t i = 0; i < topoOrder_.size(); ++i) {
        MachineBasicBlock *MBB = topoOrder_[i];
        double Cycle_bbi = getBlockCost(MBB);

        // Mandatory boundaries: loop headers and call sites
        if (MBB != entryMBB) {
            if (loopHeaders_.count(MBB) || callSiteBlocks_.count(MBB))
                boundarySet.insert(MBB);
        }

        bool isBoundary = boundarySet.count(MBB) > 0;

        double accum_cycle;
        if (isBoundary) {
            accum_cycle = Cycle_bbi;
        } else {
            accum_cycle = Cycle_bbi + incomeCycle[MBB];
        }

        // Place boundary if accumulated energy exceeds threshold
        if (accum_cycle >= E_safe_ && !isBoundary) {
            boundarySet.insert(MBB);
            accum_cycle = Cycle_bbi;
        }

        // Split oversized blocks
        while (accum_cycle >= E_safe_) {
            MachineBasicBlock *newMBB = splitBlock(MBB, E_safe_, i);
            if (!newMBB) {
                result.feasible = false;
                result.errorMessage = "Block '" + std::string(MBB->getName()) + "' (BB#" +
                                      std::to_string(MBB->getNumber()) +
                                      ") exceeds E_safe and cannot be split further";
                return result;
            }

            Cycle_bbi = getBlockCost(MBB);
            accum_cycle = Cycle_bbi;
        }

        // Propagate to successors
        for (MachineBasicBlock *succ : MBB->successors()) {
            if (incomeCycle.find(succ) == incomeCycle.end())
                incomeCycle[succ] = 0.0;
            incomeCycle[succ] = std::max(incomeCycle[succ], accum_cycle);
        }
    }

    // Build result: collect boundaries and form regions
    result.regionBoundaries.clear();
    result.regions.clear();

    MachineBasicBlock *currentRegionStart = nullptr;
    std::vector<MachineBasicBlock *> currentRegionBlocks;
    double currentRegionEnergy = 0.0;

    for (MachineBasicBlock *MBB : topoOrder_) {
        if (boundarySet.count(MBB)) {
            // Finish previous region
            if (!currentRegionBlocks.empty()) {
                MachineRegionInfo region;
                region.startBlock = currentRegionStart;
                region.blocks = currentRegionBlocks;
                region.totalEnergy = currentRegionEnergy;
                result.regions.push_back(region);
            }

            // Start new region
            result.regionBoundaries.push_back(MBB);
            currentRegionStart = MBB;
            currentRegionBlocks.clear();
            currentRegionEnergy = 0.0;
        }

        currentRegionBlocks.push_back(MBB);
        currentRegionEnergy += getBlockCost(MBB);
    }

    // Finish last region
    if (!currentRegionBlocks.empty()) {
        MachineRegionInfo region;
        region.startBlock = currentRegionStart;
        region.blocks = currentRegionBlocks;
        region.totalEnergy = currentRegionEnergy;
        result.regions.push_back(region);
    }

    return result;
}

MachineBasicBlock *RockClimbMachineOptimizer::splitBlock(MachineBasicBlock *MBB, double threshold,
                                                         size_t insertIdx) {
    // Find the split point: the instruction where cumulative energy reaches threshold
    double cumulative = 0.0;
    MachineBasicBlock::iterator splitPt = MBB->end();

    for (MachineBasicBlock::iterator I = MBB->begin(), E = MBB->end(); I != E; ++I) {
        double instCost = estimator_.estimateInstruction(*I);
        cumulative += instCost;
        if (cumulative >= threshold && std::next(I) != E) {
            splitPt = std::next(I);
            break;
        }
    }

    // Can't split if we couldn't find a valid split point
    if (splitPt == MBB->end() || splitPt == MBB->begin())
        return nullptr;

    // Use MachineBasicBlock::splitAt to create the new block
    MachineBasicBlock *newMBB = MF_.CreateMachineBasicBlock();
    MF_.insert(std::next(MachineFunction::iterator(MBB)), newMBB);

    // Move instructions from splitPt to end into newMBB
    newMBB->splice(newMBB->end(), MBB, splitPt, MBB->end());

    // Transfer successors from MBB to newMBB
    while (!MBB->succ_empty()) {
        MachineBasicBlock *succ = *MBB->succ_begin();
        MBB->removeSuccessor(succ);
        newMBB->addSuccessor(succ);
    }
    // MBB now falls through to newMBB
    MBB->addSuccessor(newMBB);

    // Update energy costs for both halves
    energyCosts_[MBB] = estimator_.estimateBlock(*MBB);
    energyCosts_[newMBB] = estimator_.estimateBlock(*newMBB);

    // Insert new block into topological order right after current
    topoOrder_.insert(topoOrder_.begin() + static_cast<long>(insertIdx) + 1, newMBB);

    // Re-check for call sites in both halves
    if (!blockHasMachineCallSite(*MBB))
        callSiteBlocks_.erase(MBB);
    if (blockHasMachineCallSite(*newMBB))
        callSiteBlocks_.insert(newMBB);

    return newMBB;
}

} // namespace checkpoint
