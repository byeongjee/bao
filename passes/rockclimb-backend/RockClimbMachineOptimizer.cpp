#include "RockClimbMachineOptimizer.h"
#include "MSP430Opcodes.h"
#include "MachineEnergyEstimator.h"
#include "MachineLivenessAnalysis.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

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
                                                     double E_safe, double checkpoint_store_energy)
    : MF_(MF), MLI_(MLI), estimator_(estimator), E_safe_(E_safe),
      checkpointStoreEnergy_(checkpoint_store_energy) {
    // Compute per-block energy costs
    for (MachineBasicBlock &MBB : MF_)
        energyCosts_[&MBB] = estimator_.estimateBlock(MBB);

    identifyLoopHeaders();
    identifyPostCallBlocks();
    computeTopologicalOrder();

    if (checkpointStoreEnergy_ > 0) {
        const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
        liveIn_ = computeBulkLiveIn(MF_, TRI);
    }
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

void RockClimbMachineOptimizer::identifyPostCallBlocks() {
    postCallBlocks_.clear();
    for (MachineBasicBlock &MBB : MF_) {
        if (blockHasMachineCallSite(MBB)) {
            for (MachineBasicBlock *succ : MBB.successors())
                postCallBlocks_.insert(succ);
        }
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

void RockClimbMachineOptimizer::collectBlockDefs(MachineBasicBlock *MBB) {
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    const MachineRegisterInfo &MRI = MF_.getRegInfo();
    for (const MachineInstr &MI : *MBB) {
        for (const MachineOperand &MO : MI.operands()) {
            if (!MO.isReg() || !MO.isDef() || !MO.getReg().isPhysical())
                continue;
            MCPhysReg reg = MO.getReg().asMCReg();
            if (MRI.isReserved(reg))
                continue;
            for (unsigned r = msp430::R4; r <= msp430::R15; ++r) {
                if (TRI->regsOverlap(reg, r)) {
                    defsInRegion_.insert(static_cast<MCPhysReg>(r));
                    break;
                }
            }
        }
    }
}

double RockClimbMachineOptimizer::computeCkptOverhead(MachineBasicBlock *MBB) const {
    if (checkpointStoreEnergy_ <= 0)
        return 0.0;
    auto it = liveIn_.find(MBB);
    if (it == liveIn_.end())
        return 0.0;
    unsigned count = 0;
    for (MCPhysReg reg : it->second) {
        if (defsInRegion_.count(reg))
            ++count;
    }
    return count * checkpointStoreEnergy_;
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

    defsInRegion_.clear();

    // Process blocks in topological order
    for (size_t i = 0; i < topoOrder_.size(); ++i) {
        MachineBasicBlock *MBB = topoOrder_[i];
        double Cycle_bbi = getBlockCost(MBB);

        // Mandatory boundaries: loop headers and post-call blocks
        if (MBB != entryMBB) {
            if (loopHeaders_.count(MBB) || postCallBlocks_.count(MBB))
                boundarySet.insert(MBB);
        }

        bool isBoundary = boundarySet.count(MBB) > 0;

        double accum_cycle;
        if (isBoundary) {
            defsInRegion_.clear();
            accum_cycle = Cycle_bbi;
        } else {
            double candidate = Cycle_bbi + incomeCycle[MBB];
            double ckptOverhead = computeCkptOverhead(MBB);

            if (candidate + ckptOverhead >= E_safe_) {
                boundarySet.insert(MBB);
                isBoundary = true;
                defsInRegion_.clear();
                accum_cycle = Cycle_bbi;
            } else {
                accum_cycle = candidate;
            }
        }

        // Single-block infeasibility check
        if (accum_cycle >= E_safe_) {
            result.feasible = false;
            result.errorMessage = "Block '" + std::string(MBB->getName()) + "' (BB#" +
                                  std::to_string(MBB->getNumber()) + ") exceeds E_safe (" +
                                  std::to_string(E_safe_) + "); block splitting is not implemented";
            return result;
        }

        collectBlockDefs(MBB);

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

} // namespace checkpoint
