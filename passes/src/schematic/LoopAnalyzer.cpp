#include "schematic/LoopAnalyzer.h"
#include "common/LoopTripCount.h"
#include "schematic/IntervalAllocator.h"
#include "schematic/RCGSolver.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <queue>
#include <set>

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

std::vector<std::vector<llvm::BasicBlock *>>
LoopAnalyzer::enumerateLoopPathsWithoutBackEdges(llvm::Loop *L) const {
    std::vector<std::vector<llvm::BasicBlock *>> paths;
    llvm::BasicBlock *header = L->getHeader();
    if (!header)
        return paths;

    // Remove back-edges for this loop and all nested loops.
    std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> backEdges;
    llvm::SmallVector<llvm::Loop *, 8> stack;
    stack.push_back(L);
    while (!stack.empty()) {
        llvm::Loop *cur = stack.pop_back_val();
        llvm::BasicBlock *curHeader = cur->getHeader();
        llvm::SmallVector<llvm::BasicBlock *, 4> latches;
        cur->getLoopLatches(latches);
        for (llvm::BasicBlock *latch : latches)
            backEdges.insert({latch, curHeader});
        for (llvm::Loop *sub : cur->getSubLoops())
            stack.push_back(sub);
    }

    std::vector<llvm::BasicBlock *> currentPath;
    std::set<llvm::BasicBlock *> visited;

    std::function<void(llvm::BasicBlock *)> dfs = [&](llvm::BasicBlock *BB) {
        if (paths.size() >= params_.maxPaths)
            return;

        visited.insert(BB);
        currentPath.push_back(BB);

        llvm::SmallVector<llvm::BasicBlock *, 4> succs;
        for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
            if (!L->contains(Succ))
                continue;
            if (backEdges.count({BB, Succ}) > 0)
                continue;
            if (visited.count(Succ))
                continue;
            succs.push_back(Succ);
        }

        if (succs.empty()) {
            paths.push_back(currentPath);
        } else {
            for (llvm::BasicBlock *Succ : succs)
                dfs(Succ);
        }

        currentPath.pop_back();
        visited.erase(BB);
    };

    dfs(header);
    if (paths.empty())
        paths.push_back({header});
    return paths;
}

bool LoopAnalyzer::placementsDiffer(
    const std::map<llvm::GlobalVariable *, Placement> &a,
    const std::map<llvm::GlobalVariable *, Placement> &b) const {

    for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
        auto ita = a.find(GV);
        auto itb = b.find(GV);
        Placement pa = (ita != a.end()) ? ita->second : Placement::NVM;
        Placement pb = (itb != b.end()) ? itb->second : Placement::NVM;
        if (pa != pb)
            return true;
    }
    return false;
}

RegionAllocation LoopAnalyzer::buildBoundaryAllocation(
    const std::map<llvm::GlobalVariable *, Placement> &placement) const {
    RegionAllocation alloc;
    alloc.placement = placement;

    unsigned vmOffset = 0;
    for (const auto &[GV, P] : placement) {
        if (P != Placement::VM)
            continue;
        alloc.vmOffsets[GV] = vmOffset;
        vmOffset += state_.getVarSizeBytes(GV);
        // Loop conditional checkpoint currently saves/restores all VM vars.
        alloc.livenessFlags[GV] = {true, true};
    }
    alloc.vmBytesUsed = vmOffset;
    return alloc;
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
    for (llvm::BasicBlock *BB : L->getBlocks())
        bodyBlocks.push_back(BB);
    if (bodyBlocks.empty())
        return true;

    // Step 1: Analyze one iteration with back-edges removed, using the same
    // RCG path analysis as acyclic regions.
    auto loopPaths = enumerateLoopPathsWithoutBackEdges(L);
    for (const auto &pathBlocks : loopPaths) {
        bool hasNew = false;
        for (llvm::BasicBlock *BB : pathBlocks) {
            auto it = solution.blockMeta.find(BB);
            if (it == solution.blockMeta.end() || !it->second.analyzed) {
                hasNew = true;
                break;
            }
        }
        if (!hasNew)
            continue;

        RCGSolver rcg(pathBlocks, state_, cfg_, params_, solution.blockMeta,
                      solution.decidedPlacements);
        auto result = rcg.solve();
        if (!result.feasible) {
            llvm::errs() << "Error: infeasible loop body path in '"
                         << header->getName() << "': " << result.errorMessage
                         << "\n";
            return false;
        }

        for (const auto &edge : result.selectedCheckpoints)
            solution.enabledCheckpoints.insert(edge);

        for (size_t i = 0; i < result.allocations.size(); ++i) {
            solution.regions.push_back(
                {result.intervalBlocks[i], result.allocations[i]});
            for (llvm::BasicBlock *BB : result.intervalBlocks[i]) {
                auto &decided = solution.decidedPlacements[BB];
                for (const auto &[gv, p] : result.allocations[i].placement)
                    decided.insert({gv, p}); // first decision wins
            }
        }

        double cumulativeEnergy = 0.0;
        for (size_t ri = 0; ri < result.intervalBlocks.size(); ++ri) {
            cumulativeEnergy = 0.0;
            for (llvm::BasicBlock *BB : result.intervalBlocks[ri]) {
                double blockE = cfg_.getBlockInfo(BB).energyCost;
                for (const auto &[gv, p] : result.allocations[ri].placement) {
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
                if (newELeft < meta.E_left)
                    meta.E_left = newELeft;
                if (newEToLeave > meta.E_to_leave)
                    meta.E_to_leave = newEToLeave;
                meta.analyzed = true;
            }
        }
    }

    // Gather boundary placements from the one-iteration analysis.
    std::map<llvm::GlobalVariable *, Placement> headerPlacement;
    std::map<llvm::GlobalVariable *, Placement> latchPlacement;
    auto hIt = solution.decidedPlacements.find(header);
    if (hIt != solution.decidedPlacements.end())
        headerPlacement = hIt->second;
    auto lIt = solution.decidedPlacements.find(latch);
    if (lIt != solution.decidedPlacements.end())
        latchPlacement = lIt->second;

    if (headerPlacement.empty())
        headerPlacement =
            computeIntervalAllocation({header}, state_, params_).placement;
    if (latchPlacement.empty())
        latchPlacement =
            computeIntervalAllocation({latch}, state_, params_).placement;

    bool mandatoryBackEdge = placementsDiffer(headerPlacement, latchPlacement);

    LoopCheckpointDecision decision;
    decision.loop = L;
    decision.bodyAllocation = buildBoundaryAllocation(headerPlacement);

    // Step 2: Decide back-edge policy.
    if (mandatoryBackEdge) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        solution.enabledCheckpoints.insert({latch, header});
        solution.loopDecisions[header] = decision;
        return true;
    }

    // Prefer paper formula based on propagated metadata when available.
    double E_loop = 0.0;
    auto hMetaIt = solution.blockMeta.find(header);
    auto lMetaIt = solution.blockMeta.find(latch);
    if (hMetaIt != solution.blockMeta.end() && lMetaIt != solution.blockMeta.end() &&
        hMetaIt->second.analyzed && lMetaIt->second.analyzed) {
        E_loop = hMetaIt->second.E_left - lMetaIt->second.E_left;
    }
    if (E_loop <= 0.0) {
        E_loop = computeMaxIterationEnergy(L, decision.bodyAllocation, solution);
    }
    decision.E_loop = E_loop;

    if (E_loop <= 0.0) {
        decision.numIterationsPerCharge = 0;
        solution.loopDecisions[header] = decision;
        return true;
    }

    // Conservative loop periodicity: reserve checkpoint cost explicitly.
    double E_ckpt = params_.E_epi + params_.E_pro +
                    params_.N_reg * (params_.regStoreEnergy +
                                     params_.regRestoreEnergy);
    for (const auto &[gv, p] : decision.bodyAllocation.placement) {
        if (p != Placement::VM)
            continue;
        unsigned varSize = state_.getVarSizeBytes(gv);
        E_ckpt += (params_.memStoreEnergyPerByte +
                   params_.memRestoreEnergyPerByte) * varSize;
    }

    double availableCapacity = params_.capacity - E_ckpt;
    unsigned numIt = (availableCapacity > 0.0)
        ? static_cast<unsigned>(std::floor(availableCapacity / E_loop))
        : 0;

    if (numIt == 0) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        solution.enabledCheckpoints.insert({latch, header});
    } else if (numIt >= maxIt) {
        decision.numIterationsPerCharge = 0;
    } else {
        decision.numIterationsPerCharge = numIt;
        solution.enabledCheckpoints.insert({latch, header});
    }

    solution.loopDecisions[header] = decision;
    return true;
}

/// Given a BasicBlock BB inside Parent, return the direct child loop of Parent
/// that contains BB, or nullptr if BB belongs directly to Parent.
static llvm::Loop *getDirectChildLoop(const llvm::Loop *Parent,
                                       const llvm::BasicBlock *BB,
                                       const llvm::LoopInfo &LI) {
    llvm::Loop *Inner = LI.getLoopFor(BB);
    if (!Inner || Inner == Parent) return nullptr;
    while (Inner->getParentLoop() != Parent)
        Inner = Inner->getParentLoop();
    return Inner;
}

double LoopAnalyzer::computeMaxIterationEnergy(
    llvm::Loop *L, const RegionAllocation &allocation,
    const SchematicSolution &solution) const {

    llvm::BasicBlock *header = L->getHeader();

    // --- Step 1: Identify collapsible sub-loops ---
    // Inner loops with numIterationsPerCharge==0 (entire loop fits in one charge)
    // must be collapsed: their energy contribution is E_loop * tripCount, not
    // just the energy of a single traversal of their blocks.
    llvm::DenseMap<llvm::Loop *, double> collapsedEnergy;
    llvm::DenseSet<llvm::BasicBlock *> skipBlocks;

    for (llvm::Loop *ChildL : L->getSubLoops()) {
        llvm::BasicBlock *childHeader = ChildL->getHeader();
        auto it = solution.loopDecisions.find(childHeader);
        if (it == solution.loopDecisions.end()) continue;
        const LoopCheckpointDecision &childDecision = it->second;

        if (childDecision.numIterationsPerCharge == 0) {
            // Entire inner loop fits in one charge — collapse with full trip count
            auto childTC = getMaxTripCount(ChildL);
            uint64_t tc = childTC.value_or(1);
            collapsedEnergy[ChildL] = childDecision.E_loop * tc;
        } else {
            // Inner loop has checkpoints — collapse with K (worst-case segment)
            collapsedEnergy[ChildL] = childDecision.E_loop *
                                       childDecision.numIterationsPerCharge;
        }

        // Skip non-header body blocks for all collapsed loops
        for (llvm::BasicBlock *BB : ChildL->getBlocks()) {
            if (BB != childHeader)
                skipBlocks.insert(BB);
        }
    }

    // --- Step 2: Build active block set and energy map ---
    // Energy lambda: collapsed inner-loop headers get collapsed energy;
    // other blocks get base + NVM penalty energy.
    auto getBlockEnergy = [&](llvm::BasicBlock *BB) -> double {
        llvm::Loop *ChildL = getDirectChildLoop(L, BB, LI_);
        if (ChildL && ChildL->getHeader() == BB) {
            auto it = collapsedEnergy.find(ChildL);
            if (it != collapsedEnergy.end())
                return it->second;
        }
        double E = cfg_.getBlockInfo(BB).energyCost;
        for (const auto &[gv, p] : allocation.placement) {
            if (p == Placement::NVM) {
                unsigned accesses = state_.getLoadCount(BB, gv) +
                                    state_.getStoreCount(BB, gv);
                E += accesses * params_.nvmAccessPenalty;
            }
        }
        return E;
    };

    // Active blocks: L's blocks minus skipped inner-loop body blocks
    std::vector<llvm::BasicBlock *> activeBlocks;
    for (llvm::BasicBlock *BB : L->getBlocks()) {
        if (!skipBlocks.count(BB))
            activeBlocks.push_back(BB);
    }

    llvm::DenseSet<llvm::BasicBlock *> activeSet(activeBlocks.begin(),
                                                  activeBlocks.end());

    // Successor lambda: collapsed inner-loop headers jump to exit blocks;
    // other blocks use normal successors filtered to active set.
    auto getSuccessors = [&](llvm::BasicBlock *BB,
                             bool isLatch) -> llvm::SmallVector<llvm::BasicBlock *, 4> {
        llvm::SmallVector<llvm::BasicBlock *, 4> succs;
        llvm::Loop *ChildL = getDirectChildLoop(L, BB, LI_);
        if (ChildL && ChildL->getHeader() == BB && collapsedEnergy.count(ChildL)) {
            // Collapsed inner loop: successors are the inner loop's exit blocks
            llvm::SmallVector<llvm::BasicBlock *, 4> exits;
            ChildL->getExitBlocks(exits);
            for (llvm::BasicBlock *Exit : exits) {
                if (L->contains(Exit) && activeSet.count(Exit))
                    succs.push_back(Exit);
            }
        } else {
            for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
                if (isLatch && Succ == header) continue; // skip back-edge
                if (L->contains(Succ) && activeSet.count(Succ))
                    succs.push_back(Succ);
            }
        }
        return succs;
    };

    // --- Step 3: Topological sort and longest-path DP ---
    // Identify back-edge targets for exclusion
    llvm::SmallVector<llvm::BasicBlock *, 4> latches;
    L->getLoopLatches(latches);
    llvm::DenseSet<llvm::BasicBlock *> latchSet(latches.begin(), latches.end());

    // Compute in-degree within active blocks (excluding back-edges)
    llvm::DenseMap<llvm::BasicBlock *, unsigned> inDegree;
    for (llvm::BasicBlock *BB : activeBlocks)
        inDegree[BB] = 0;

    for (llvm::BasicBlock *BB : activeBlocks) {
        bool isLatch = latchSet.count(BB);
        for (llvm::BasicBlock *Succ : getSuccessors(BB, isLatch))
            inDegree[Succ]++;
    }

    // Topological sort via Kahn's algorithm
    std::queue<llvm::BasicBlock *> worklist;
    for (llvm::BasicBlock *BB : activeBlocks) {
        if (inDegree[BB] == 0)
            worklist.push(BB);
    }

    std::vector<llvm::BasicBlock *> topoOrder;
    while (!worklist.empty()) {
        llvm::BasicBlock *BB = worklist.front();
        worklist.pop();
        topoOrder.push_back(BB);

        bool isLatch = latchSet.count(BB);
        for (llvm::BasicBlock *Succ : getSuccessors(BB, isLatch)) {
            if (--inDegree[Succ] == 0)
                worklist.push(Succ);
        }
    }

    // DP forward: longest path cost from header
    llvm::DenseMap<llvm::BasicBlock *, double> maxCost;
    for (llvm::BasicBlock *BB : activeBlocks)
        maxCost[BB] = -1.0; // unvisited

    maxCost[header] = getBlockEnergy(header);

    for (llvm::BasicBlock *BB : topoOrder) {
        if (maxCost[BB] < 0.0) continue; // unreachable from header

        bool isLatch = latchSet.count(BB);
        for (llvm::BasicBlock *Succ : getSuccessors(BB, isLatch)) {
            double candidate = maxCost[BB] + getBlockEnergy(Succ);
            if (candidate > maxCost[Succ])
                maxCost[Succ] = candidate;
        }
    }

    // Return max cost at any latch (worst case across all latches)
    double result = 0.0;
    for (llvm::BasicBlock *Latch : latches) {
        if (maxCost.count(Latch) && maxCost[Latch] > result)
            result = maxCost[Latch];
    }
    return result;
}

} // namespace checkpoint
