#include "milp/EnergyPathUtils.h"

#include "common/LoopTripCount.h"
#include "common/LoopUtils.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"

#include <functional>

using namespace llvm;

namespace checkpoint {

namespace {

enum class VisitState {
    Unvisited = 0,
    Visiting,
    Visited,
};

} // namespace

WorstCasePath computeWorstCasePath(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
                                   LoopInfo &LI, ScalarEvolution &SE,
                                   function_ref<std::optional<uint64_t>(Loop *)> subLoopTripCount) {
    WorstCasePath result;

    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    if (!Header || !Latch) {
        result.error = "missing-header-or-latch";
        return result;
    }

    DenseMap<const Loop *, double> subLoopTotal;
    DenseMap<const Loop *, SmallPtrSet<const BasicBlock *, 16>> subLoopBlocks;
    for (Loop *SubL : L->getSubLoops()) {
        std::optional<uint64_t> subTC = subLoopTripCount(SubL);
        if (!subTC) {
            result.error = "sub-loop-unknown-trip-count";
            return result;
        }

        auto subLoopPath = computeWorstCasePath(SubL, blockEnergy, LI, SE, subLoopTripCount);
        if (!subLoopPath.ok) {
            result.error = "sub-loop-energy-unavailable";
            return result;
        }

        subLoopTotal[SubL] = subLoopPath.energy * static_cast<double>(*subTC);
        for (const BasicBlock *BB : SubL->blocks())
            subLoopBlocks[SubL].insert(BB);
    }

    auto getEnergy = [&](const BasicBlock *BB) -> double {
        Loop *ChildL = getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            auto it = subLoopTotal.find(ChildL);
            return (it != subLoopTotal.end()) ? it->second : 0.0;
        }
        auto it = blockEnergy.find(BB);
        return (it != blockEnergy.end()) ? it->second : 0.0;
    };

    auto getSuccs = [&](const BasicBlock *BB) -> SmallVector<const BasicBlock *, 4> {
        SmallVector<const BasicBlock *, 4> succs;
        Loop *ChildL = getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            SmallVector<BasicBlock *, 4> exits;
            ChildL->getExitBlocks(exits);
            for (BasicBlock *Exit : exits)
                succs.push_back(Exit);
        } else {
            for (const BasicBlock *Succ : successors(BB))
                succs.push_back(Succ);
        }
        return succs;
    };

    if (Header == Latch) {
        result.ok = true;
        result.energy = getEnergy(Header);
        result.blocksOnPath.insert(Header);
        return result;
    }

    DenseMap<const BasicBlock *, VisitState> visitState;
    DenseMap<const BasicBlock *, double> memo;
    DenseMap<const BasicBlock *, const BasicBlock *> bestSucc;
    bool cycleDetected = false;

    std::function<double(const BasicBlock *)> dfs = [&](const BasicBlock *BB) -> double {
        if (BB == Latch) {
            return getEnergy(BB);
        }

        VisitState state = visitState.lookup(BB);
        if (state == VisitState::Visiting) {
            cycleDetected = true;
            return -1.0;
        }
        if (state == VisitState::Visited) {
            return memo[BB];
        }

        visitState[BB] = VisitState::Visiting;
        double bestSuccEnergy = -1.0;
        const BasicBlock *best = nullptr;
        for (const BasicBlock *Succ : getSuccs(BB)) {
            if (!L->contains(Succ)) {
                continue;
            }
            if (BB == Latch && Succ == Header) {
                continue;
            }
            double succEnergy = dfs(Succ);
            if (succEnergy < 0.0) {
                continue;
            }
            if (succEnergy > bestSuccEnergy) {
                bestSuccEnergy = succEnergy;
                best = Succ;
            }
        }
        visitState[BB] = VisitState::Visited;

        if (!best || bestSuccEnergy < 0.0) {
            memo[BB] = -1.0;
            return -1.0;
        }

        bestSucc[BB] = best;
        memo[BB] = getEnergy(BB) + bestSuccEnergy;
        return memo[BB];
    };

    double energy = dfs(Header);
    if (cycleDetected) {
        result.error = "unsupported-intra-loop-cycle";
        return result;
    }
    if (energy <= 0.0) {
        result.error = "unable-to-compute-path-energy";
        return result;
    }

    SmallPtrSet<const BasicBlock *, 16> pathBlocks;
    const BasicBlock *cur = Header;
    while (cur) {
        pathBlocks.insert(cur);
        Loop *ChildL = getDirectChildLoop(L, cur, LI);
        if (ChildL && ChildL->getHeader() == cur) {
            auto it = subLoopBlocks.find(ChildL);
            if (it != subLoopBlocks.end()) {
                pathBlocks.insert(it->second.begin(), it->second.end());
            }
        }
        if (cur == Latch) {
            break;
        }
        auto it = bestSucc.find(cur);
        if (it == bestSucc.end()) {
            result.error = "path-reconstruction-failed";
            return result;
        }
        cur = it->second;
    }

    if (pathBlocks.empty() || !pathBlocks.count(Latch)) {
        result.error = "path-reconstruction-failed";
        return result;
    }

    result.ok = true;
    result.energy = energy;
    result.blocksOnPath = std::move(pathBlocks);
    return result;
}

std::optional<uint64_t> summarySubLoopTripCount(Loop *SubL, ScalarEvolution &SE) {
    unsigned scevTC = SE.getSmallConstantTripCount(SubL);
    auto markerTC = getMarkerTripCount(SubL);

    if (scevTC > 0 && markerTC)
        return std::min<uint64_t>(scevTC, *markerTC);
    if (scevTC > 0)
        return scevTC;
    return markerTC;
}

WorstCasePath computeWorstCaseSummaryPath(Loop *L,
                                          const DenseMap<const BasicBlock *, double> &blockEnergy,
                                          LoopInfo &LI, ScalarEvolution &SE) {
    return computeWorstCasePath(L, blockEnergy, LI, SE,
                                [&](Loop *SubL) { return summarySubLoopTripCount(SubL, SE); });
}

SummaryBudget computeSummaryBudget(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
                                   LoopInfo &LI, ScalarEvolution &SE, const StateAnalysis &state,
                                   const MILPEnergyParams &params) {
    SummaryBudget summaryBudget;

    double budget = params.capacity - params.E_pro - params.E_epi;
    if (budget <= 0.0) {
        summaryBudget.error = "nonpositive-energy-budget";
        return summaryBudget;
    }

    summaryBudget.worstCasePath = computeWorstCaseSummaryPath(L, blockEnergy, LI, SE);
    if (!summaryBudget.worstCasePath.ok) {
        summaryBudget.error = summaryBudget.worstCasePath.error.empty()
                                  ? "unknown-path-summary-error"
                                  : summaryBudget.worstCasePath.error;
        return summaryBudget;
    }
    if (summaryBudget.worstCasePath.energy <= 0.0) {
        summaryBudget.error = "nonpositive-loop-energy";
        return summaryBudget;
    }

    // Must match EnergyModel::computeNvmPenalties.
    auto nvmPenaltyIn = [&](const BasicBlock *BB) {
        double penalty = 0.0;
        for (GlobalVariable *GV : state.getVMObjs()) {
            unsigned accesses = state.getLoadCount(BB, GV) + state.getStoreCount(BB, GV);
            penalty += static_cast<double>(accesses) * params.nvmAccessPenalty;
        }
        return penalty;
    };
    for (const BasicBlock *BB : summaryBudget.worstCasePath.blocksOnPath)
        summaryBudget.perIterNvmPenalty += nvmPenaltyIn(BB);

    BasicBlock *preheaderBB = L->getLoopPreheader();
    if (preheaderBB) {
        summaryBudget.preheaderEnergy = blockEnergy.lookup(preheaderBB);
        summaryBudget.preheaderNvmPenalty = nvmPenaltyIn(preheaderBB);
    }

    SmallPtrSet<const BasicBlock *, 16> marginBlocks(
        summaryBudget.worstCasePath.blocksOnPath.begin(),
        summaryBudget.worstCasePath.blocksOnPath.end());
    if (preheaderBB)
        marginBlocks.insert(preheaderBB);
    summaryBudget.boundaryStateMargin = computeBoundaryStateMarginOnPath(
        marginBlocks, state, params, summaryBudget.restoreLiveInMargin,
        summaryBudget.commitDefMargin);

    summaryBudget.budgetAfterBoundary = budget - summaryBudget.boundaryStateMargin;
    summaryBudget.ok = true;
    return summaryBudget;
}

} // namespace checkpoint
