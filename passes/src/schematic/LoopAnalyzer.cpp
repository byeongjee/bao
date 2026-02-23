#include "schematic/LoopAnalyzer.h"
#include "common/LoopTripCount.h"
#include "schematic/IntervalAllocator.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace checkpoint {

LoopAnalyzer::LoopAnalyzer(llvm::LoopInfo &LI,
                           llvm::ScalarEvolution &SE,
                           const CFGAnalysis &cfg,
                           const StateAnalysis &state,
                           const SchematicParams &params)
    : LI_(LI), SE_(SE), cfg_(cfg), state_(state), params_(params) {}

std::optional<uint64_t> LoopAnalyzer::getMaxTripCount(llvm::Loop *L) const {
    // Try annotation first
    auto markerTC = getMarkerTripCount(L);
    if (markerTC)
        return markerTC;

    // Fallback to ScalarEvolution
    unsigned seTC = SE_.getSmallConstantMaxTripCount(L);
    if (seTC > 0)
        return static_cast<uint64_t>(seTC);

    return std::nullopt;
}

bool LoopAnalyzer::analyzeLoops(SchematicSolution &solution) {
    // Get loops in preorder and reverse for bottom-up (inner first)
    auto loops = LI_.getLoopsInPreorder();
    std::reverse(loops.begin(), loops.end());

    for (llvm::Loop *L : loops) {
        if (!analyzeLoop(L, solution))
            return false;
    }
    return true;
}

bool LoopAnalyzer::analyzeLoop(llvm::Loop *L, SchematicSolution &solution) {
    llvm::BasicBlock *header = L->getHeader();
    llvm::BasicBlock *latch = L->getLoopLatch();

    if (!header || !latch) return true; // Skip multi-latch loops (not an error)

    // All loops must have a trip count bound (__loop_tripcount or SE)
    auto maxItOpt = getMaxTripCount(L);
    if (!maxItOpt) {
        llvm::errs() << "Error: loop at '" << header->getName()
                     << "' has no trip count bound (__loop_tripcount annotation "
                     << "or ScalarEvolution). Aborting SCHEMATIC.\n";
        return false;
    }
    uint64_t maxIt = *maxItOpt;

    // Collect loop body blocks
    std::vector<llvm::BasicBlock *> bodyBlocks;
    for (llvm::BasicBlock *BB : L->getBlocks()) {
        bodyBlocks.push_back(BB);
    }

    if (bodyBlocks.empty()) return true;

    // Analyze one iteration: compute allocation
    auto bodyAllocation = computeIntervalAllocation(
        bodyBlocks, state_, params_);

    // Since we use a single allocation for the whole body, alloc(H) == alloc(L).
    // Mandatory back-edge only needed if per-block allocations differ.
    bool mandatoryBackEdge = false;

    LoopCheckpointDecision decision;
    decision.loop = L;
    decision.bodyAllocation = bodyAllocation;

    if (mandatoryBackEdge) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        solution.enabledCheckpoints.insert({latch, header});
    } else {
        // Compute per-iteration energy via longest path in loop body DAG
        double E_loop = computeMaxIterationEnergy(L, bodyAllocation);
        decision.E_loop = E_loop;

        if (E_loop <= 0.0) {
            // Empty or zero-energy loop body
            decision.numIterationsPerCharge = 0;
        } else {
            // Reserve energy for the checkpoint itself:
            // epilogue + prologue + register save/restore + VM memory save/restore
            double E_ckpt = params_.E_epi + params_.E_pro +
                            params_.N_reg * (params_.regStoreEnergy +
                                             params_.regRestoreEnergy);
            for (const auto &[gv, p] : bodyAllocation.placement) {
                if (p == Placement::VM) {
                    unsigned varSize = state_.getVarSizeBytes(gv);
                    E_ckpt += (params_.memStoreEnergyPerByte +
                               params_.memRestoreEnergyPerByte) * varSize;
                }
            }
            double availableCapacity = params_.capacity - E_ckpt;
            unsigned numIt = (availableCapacity > 0.0)
                ? static_cast<unsigned>(std::floor(availableCapacity / E_loop))
                : 0;

            if (numIt == 0) {
                // Single iteration doesn't fit — force checkpoint every iteration
                decision.mandatoryBackEdge = true;
                decision.numIterationsPerCharge = 1;
                solution.enabledCheckpoints.insert({latch, header});
            } else if (numIt >= maxIt) {
                // Entire loop fits in one charge — no checkpoint needed
                decision.numIterationsPerCharge = 0;
            } else {
                // Conditional checkpoint every numIt iterations
                decision.numIterationsPerCharge = numIt;
                solution.enabledCheckpoints.insert({latch, header});
            }
        }
    }

    solution.loopDecisions[header] = decision;

    // Update blockMeta for loop body blocks
    double cumulativeEnergy = 0.0;
    for (llvm::BasicBlock *BB : bodyBlocks) {
        double blockE = cfg_.getBlockInfo(BB).energyCost;
        // Add NVM penalties
        for (const auto &[gv, p] : bodyAllocation.placement) {
            if (p == Placement::NVM) {
                unsigned accesses = state_.getLoadCount(BB, gv) +
                                    state_.getStoreCount(BB, gv);
                blockE += accesses * params_.nvmAccessPenalty;
            }
        }

        cumulativeEnergy += blockE;

        auto &meta = solution.blockMeta[BB];
        double newELeft = params_.capacity - cumulativeEnergy;
        double newEToLeave = cumulativeEnergy;

        // Monotonic updates: E_left can only decrease, E_to_leave can only increase
        if (newELeft < meta.E_left)
            meta.E_left = newELeft;
        if (newEToLeave > meta.E_to_leave)
            meta.E_to_leave = newEToLeave;
        meta.analyzed = true;
    }

    return true;
}

double LoopAnalyzer::computeMaxIterationEnergy(
    llvm::Loop *L, const RegionAllocation &allocation) const {

    // Compute per-block execution energy (base + NVM penalties) under allocation
    llvm::DenseMap<llvm::BasicBlock *, double> blockEnergy;
    for (llvm::BasicBlock *BB : L->getBlocks()) {
        double E = cfg_.getBlockInfo(BB).energyCost;
        for (const auto &[gv, p] : allocation.placement) {
            if (p == Placement::NVM) {
                unsigned accesses = state_.getLoadCount(BB, gv) +
                                    state_.getStoreCount(BB, gv);
                E += accesses * params_.nvmAccessPenalty;
            }
        }
        blockEnergy[BB] = E;
    }

    // Identify back-edge targets for exclusion
    llvm::SmallVector<llvm::BasicBlock *, 4> latches;
    L->getLoopLatches(latches);
    llvm::DenseSet<llvm::BasicBlock *> latchSet(latches.begin(), latches.end());
    llvm::BasicBlock *header = L->getHeader();

    // Compute in-degree within loop body (excluding back-edges)
    llvm::DenseMap<llvm::BasicBlock *, unsigned> inDegree;
    for (llvm::BasicBlock *BB : L->getBlocks())
        inDegree[BB] = 0;

    for (llvm::BasicBlock *BB : L->getBlocks()) {
        // Skip back-edges: latch -> header
        if (latchSet.count(BB)) {
            for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                if (Succ == header) continue; // back-edge, skip
                if (L->contains(Succ))
                    inDegree[Succ]++;
            }
        } else {
            for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                if (L->contains(Succ))
                    inDegree[Succ]++;
            }
        }
    }

    // Topological sort via Kahn's algorithm
    std::queue<llvm::BasicBlock *> worklist;
    for (llvm::BasicBlock *BB : L->getBlocks()) {
        if (inDegree[BB] == 0)
            worklist.push(BB);
    }

    std::vector<llvm::BasicBlock *> topoOrder;
    while (!worklist.empty()) {
        llvm::BasicBlock *BB = worklist.front();
        worklist.pop();
        topoOrder.push_back(BB);

        bool isLatch = latchSet.count(BB);
        for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
            if (isLatch && Succ == header) continue; // skip back-edge
            if (!L->contains(Succ)) continue;
            if (--inDegree[Succ] == 0)
                worklist.push(Succ);
        }
    }

    // DP forward: longest path cost from header
    llvm::DenseMap<llvm::BasicBlock *, double> maxCost;
    for (llvm::BasicBlock *BB : L->getBlocks())
        maxCost[BB] = -1.0; // unvisited

    maxCost[header] = blockEnergy[header];

    for (llvm::BasicBlock *BB : topoOrder) {
        if (maxCost[BB] < 0.0) continue; // unreachable from header

        bool isLatch = latchSet.count(BB);
        for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
            if (isLatch && Succ == header) continue; // skip back-edge
            if (!L->contains(Succ)) continue;

            double candidate = maxCost[BB] + blockEnergy[Succ];
            if (candidate > maxCost[Succ])
                maxCost[Succ] = candidate;
        }
    }

    // Return max cost at any latch (worst case across all latches)
    double result = 0.0;
    for (llvm::BasicBlock *Latch : latches) {
        if (maxCost[Latch] > result)
            result = maxCost[Latch];
    }
    return result;
}

} // namespace checkpoint
