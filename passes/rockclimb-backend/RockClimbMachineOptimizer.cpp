#include "RockClimbMachineOptimizer.h"
#include "MSP430Constants.h"
#include "MachineDistributedCheckpointing.h"
#include "MachineEnergyEstimator.h"
#include "MachineLivenessAnalysis.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

#include <algorithm>
#include <queue>

using namespace llvm;

namespace checkpoint {

RockClimbMachineOptimizer::RockClimbMachineOptimizer(MachineFunction &MF, MachineLoopInfo &MLI,
                                                     const MachineEnergyEstimator &estimator,
                                                     double E_safe, double reg_store_energy)
    : MF_(MF), MLI_(MLI), estimator_(estimator), E_safe_(E_safe), regStoreEnergy_(reg_store_energy),
      C_(MSP430Constants::resolve(MF)) {
    // Compute per-block energy costs
    for (MachineBasicBlock &MBB : MF_)
        energyCosts_[&MBB] = estimator_.estimateBlock(MBB);

    identifyLoopHeaders();
    identifyExitBlocks();
    computeTopologicalOrder();

    if (regStoreEnergy_ > 0)
        addDistributedSaveCosts();
}

void RockClimbMachineOptimizer::addDistributedSaveCosts() {
    // Algorithm 1 line 2 (PFI paper): Cycle_bb = Cycle_ori_bb + CkptCycles_bb.
    // The distributed checkpoint stores execute inside the blocks they are
    // inserted into, so their energy must be part of those blocks' costs —
    // that is what charges them to the region that actually pays for them at
    // runtime.
    //
    // The exact save placement depends on the region boundaries, which do not
    // exist yet, but a boundary-independent upper bound does: whatever the
    // boundary set turns out to be, register R gets at most ONE save in block
    // B — at R's last def-like point (def, or a call that may clobber R's
    // __nvm_regs slot) — and only if R is live-out of B (the saved value must
    // reach a later boundary). Charging that superset keeps every region's
    // accounted cost >= its real cost.
    //
    // Not modeled: the callee-entry argument saves, which execute in the
    // caller's region (cross-function; covered by the per-boundary E_pro
    // convention).
    const TargetRegisterInfo *TRI = MF_.getSubtarget().getRegisterInfo();
    const MachineRegisterInfo &MRI = MF_.getRegInfo();
    auto liveIn = computeBulkLiveIn(MF_, TRI);

    for (MachineBasicBlock &MBB : MF_) {
        // liveOut = union of successor live-ins
        SmallSet<MCPhysReg, 12> liveOut;
        for (const MachineBasicBlock *succ : MBB.successors())
            for (MCPhysReg reg : liveIn[succ])
                liveOut.insert(reg);
        if (liveOut.empty())
            continue;

        // Def-like points: register defs, and clobbering calls (which get a
        // post-call re-save of any live register, so they count for all).
        bool hasClobberingCall = false;
        SmallSet<MCPhysReg, 12> defined;
        for (const MachineInstr &MI : MBB) {
            if (MI.isCall() && callMayClobberNvmRegs(MI))
                hasClobberingCall = true;
            for (const MachineOperand &MO : MI.operands()) {
                if (!MO.isReg() || !MO.isDef() || !MO.getReg().isPhysical())
                    continue;
                MCPhysReg reg = MO.getReg().asMCReg();
                if (MRI.isReserved(reg))
                    continue;
                for (unsigned r = C_.R4.id(); r <= C_.R15.id(); ++r) {
                    if (TRI->regsOverlap(reg, r)) {
                        defined.insert(static_cast<MCPhysReg>(r));
                        break;
                    }
                }
            }
        }

        unsigned saves = 0;
        for (MCPhysReg reg : liveOut) {
            if (hasClobberingCall || defined.count(reg))
                ++saves;
        }
        energyCosts_[&MBB] += saves * regStoreEnergy_;
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

void RockClimbMachineOptimizer::identifyExitBlocks() {
    // Faithful to the PFI/ROCKCLIMB region-formation algorithm: boundaries are
    // placed at every function's entry and exit points. The entry boundary is
    // added in partitionRegions(); here we mark every return block as a
    // mandatory boundary. Calls are NOT boundary sites in the caller — each
    // callee is bracketed by its own entry/exit boundaries instead.
    exitBlocks_.clear();
    for (MachineBasicBlock &MBB : MF_) {
        if (MBB.isReturnBlock())
            exitBlocks_.insert(&MBB);
    }
}

void RockClimbMachineOptimizer::computeTopologicalOrder() {
    topoOrder_.clear();
    retreatingTargets_.clear();
    if (MF_.empty())
        return;

    // Reverse post-order: all predecessors (modulo retreating edges) visited
    // first. The income propagation only follows forward edges, which is safe
    // exactly when every retreating edge lands on a boundary (accumulation
    // resets there).
    SmallPtrSet<MachineBasicBlock *, 32> visited;
    DenseMap<const MachineBasicBlock *, unsigned> rpoIndex;
    ReversePostOrderTraversal<MachineFunction *> RPOT(&MF_);
    for (MachineBasicBlock *MBB : RPOT) {
        rpoIndex[MBB] = static_cast<unsigned>(topoOrder_.size());
        topoOrder_.push_back(MBB);
        visited.insert(MBB);
    }

    // Force every retreating-edge target to be a boundary. In a reducible CFG
    // retreating edges are exactly the back edges, whose targets are natural-
    // loop headers and mandatory boundaries already — no behavior change. But
    // MachineLoopInfo does not recognize irreducible cycles (e.g. a goto into
    // a loop body), and any cycle crosses at least one retreating edge, so
    // this keeps every cycle's energy bounded by a boundary.
    for (MachineBasicBlock *MBB : topoOrder_) {
        for (MachineBasicBlock *succ : MBB->successors()) {
            auto it = rpoIndex.find(succ);
            if (it != rpoIndex.end() && it->second <= rpoIndex[MBB])
                retreatingTargets_.insert(succ);
        }
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
    // The program-entry function ("main") needs no entry boundary: a power
    // failure in its first region is recovered by a normal cold boot (the
    // runtime re-runs from _start, and that first region is idempotent setup).
    // An entry checkpoint there is redundant and, on the debug device, executes
    // before debug_init's __nvm_done readback guard — which exists because
    // mspdebug's tilib driver resets the target on connect — corrupting NVM
    // readback. Callees still get entry boundaries (PFI call model).
    const bool entryIsBoundary = MF_.getName() != "main";
    if (entryIsBoundary)
        boundarySet.insert(entryMBB);

    // Process blocks in topological order
    for (size_t i = 0; i < topoOrder_.size(); ++i) {
        MachineBasicBlock *MBB = topoOrder_[i];
        double Cycle_bbi = getBlockCost(MBB);

        // Mandatory boundaries: loop headers, retreating-edge targets
        // (irreducible cycle entries), and function exit (return) blocks.
        // (The entry block is already a boundary, inserted above.)
        if (MBB != entryMBB) {
            if (loopHeaders_.count(MBB) || retreatingTargets_.count(MBB) || exitBlocks_.count(MBB))
                boundarySet.insert(MBB);
        }

        bool isBoundary = boundarySet.count(MBB) > 0;

        double accum_cycle;
        if (isBoundary) {
            accum_cycle = Cycle_bbi;
        } else {
            // Block costs already include the distributed checkpoint stores
            // (addDistributedSaveCosts), so accum is the region's real energy
            // and no separate save-overhead term is needed here.
            double candidate = Cycle_bbi + incomeCycle[MBB];
            if (candidate >= E_safe_) {
                boundarySet.insert(MBB);
                isBoundary = true;
                accum_cycle = Cycle_bbi;
            } else {
                accum_cycle = candidate;
            }
        }

        // Single-block infeasibility: one block (e.g. an expensive external
        // call such as integer division, or an over-large unrolled body) costs
        // more than a full charge can afford. Such a block cannot be made
        // power-failure-immune, so report it.
        if (accum_cycle >= E_safe_) {
            result.feasible = false;
            result.errorMessage = "Block '" + std::string(MBB->getName()) + "' (BB#" +
                                  std::to_string(MBB->getNumber()) + ") exceeds E_safe (" +
                                  std::to_string(E_safe_) +
                                  "); single block does not fit one charge";
            return result;
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

    // Region 0 starts at the entry block even when the entry is not itself a
    // boundary (program-entry function); its recovery is the cold-boot path.
    MachineBasicBlock *currentRegionStart = entryMBB;
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
