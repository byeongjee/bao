#include "milp/StripMiningSummary.h"

#include "common/LoopTripCount.h"
#include "common/LoopUtils.h"
#include "milp/EnergyPathUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/GlobalVariable.h"

#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <string>

using namespace llvm;

namespace checkpoint {

namespace {

enum class VisitState {
    Unvisited = 0,
    Visiting,
    Visited,
};

static double computePerIterNvmPenalty(const SmallPtrSetImpl<const BasicBlock *> &pathBlocks,
                                       const StateAnalysis &state, const MILPEnergyParams &params) {
    double nvmPenalty = 0.0;
    for (const BasicBlock *BB : pathBlocks) {
        for (GlobalVariable *GV : state.getVMObjs()) {
            unsigned accesses = state.getLoadCount(BB, GV) + state.getStoreCount(BB, GV);
            nvmPenalty += static_cast<double>(accesses) * params.nvmAccessPenalty;
        }
    }
    return nvmPenalty;
}

} // namespace

WorstCasePathResult
computeStripMiningSummaryPath(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergyByBB,
                              LoopInfo &LI, ScalarEvolution &SE) {
    WorstCasePathResult result;

    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    if (!Header || !Latch) {
        result.error = "missing-header-or-latch";
        return result;
    }

    DenseMap<const Loop *, double> subLoopTotal;
    DenseMap<const Loop *, SmallPtrSet<const BasicBlock *, 16>> subLoopBlocks;
    for (Loop *SubL : L->getSubLoops()) {
        auto subPath = computeStripMiningSummaryPath(SubL, blockEnergyByBB, LI, SE);
        if (!subPath.ok) {
            result.error = "sub-loop-energy-unavailable";
            return result;
        }
        unsigned scevTC = SE.getSmallConstantTripCount(SubL);
        auto markerTC = getMarkerTripCount(SubL);
        unsigned tc = 0;
        if (scevTC > 0 && markerTC)
            tc = std::min(scevTC, static_cast<unsigned>(*markerTC));
        else if (scevTC > 0)
            tc = scevTC;
        else if (markerTC)
            tc = static_cast<unsigned>(*markerTC);
        else {
            result.error = "sub-loop-unknown-trip-count";
            return result;
        }
        subLoopTotal[SubL] = subPath.energy * static_cast<double>(tc);
        for (const BasicBlock *BB : SubL->blocks())
            subLoopBlocks[SubL].insert(BB);
    }

    auto getEnergy = [&](const BasicBlock *BB) -> double {
        Loop *ChildL = getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            auto it = subLoopTotal.find(ChildL);
            return (it != subLoopTotal.end()) ? it->second : 0.0;
        }
        auto it = blockEnergyByBB.find(BB);
        return (it != blockEnergyByBB.end()) ? it->second : 0.0;
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
        if (BB == Latch)
            return getEnergy(BB);

        VisitState state = visitState.lookup(BB);
        if (state == VisitState::Visiting) {
            cycleDetected = true;
            return -1.0;
        }
        if (state == VisitState::Visited)
            return memo[BB];

        visitState[BB] = VisitState::Visiting;
        double bestSuccEnergy = -1.0;
        const BasicBlock *best = nullptr;
        for (const BasicBlock *Succ : getSuccs(BB)) {
            if (!L->contains(Succ))
                continue;
            if (BB == Latch && Succ == Header)
                continue;
            double succEnergy = dfs(Succ);
            if (succEnergy < 0.0)
                continue;
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
    const BasicBlock *Cur = Header;
    while (Cur) {
        pathBlocks.insert(Cur);
        Loop *ChildL = getDirectChildLoop(L, Cur, LI);
        if (ChildL && ChildL->getHeader() == Cur) {
            auto it = subLoopBlocks.find(ChildL);
            if (it != subLoopBlocks.end())
                pathBlocks.insert(it->second.begin(), it->second.end());
        }
        if (Cur == Latch)
            break;
        auto it = bestSucc.find(Cur);
        if (it == bestSucc.end()) {
            result.error = "path-reconstruction-failed";
            return result;
        }
        Cur = it->second;
    }

    result.ok = true;
    result.energy = energy;
    result.blocksOnPath = std::move(pathBlocks);
    return result;
}

StripMiningBudgetResult
computeStripMiningBudgetResult(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergyByBB,
                               LoopInfo &LI, ScalarEvolution &SE, const StateAnalysis &state,
                               const MILPEnergyParams &params) {
    StripMiningBudgetResult result;

    const double budget = params.capacity - params.E_pro - params.E_epi;
    if (budget <= 0.0) {
        result.reason = "nonpositive-energy-budget";
        result.details = "capacity=" + std::to_string(params.capacity) +
                         ", E_pro=" + std::to_string(params.E_pro) +
                         ", E_epi=" + std::to_string(params.E_epi) +
                         ", budget=" + std::to_string(budget);
        return result;
    }

    WorstCasePathResult path = computeStripMiningSummaryPath(L, blockEnergyByBB, LI, SE);
    if (!path.ok) {
        result.reason = path.error.empty() ? "unknown-path-summary-error" : path.error;
        result.details = "path-energy-unavailable";
        return result;
    }
    if (path.energy <= 0.0) {
        result.reason = "nonpositive-loop-energy";
        result.details = "path-energy=" + std::to_string(path.energy);
        return result;
    }

    result.pathEnergy = path.energy;
    result.pathBlocks = path.blocksOnPath;
    result.perIterNvmPenalty = computePerIterNvmPenalty(path.blocksOnPath, state, params);
    result.boundaryStateMargin = computeBoundaryStateMarginOnPath(
        path.blocksOnPath, state, params, result.restoreLiveInMargin, result.commitDefMargin);
    result.budgetAfterBoundary = budget - result.boundaryStateMargin;
    if (result.budgetAfterBoundary <= 0.0) {
        result.reason = "nonpositive-effective-budget";
        result.details = "path-energy=" + std::to_string(result.pathEnergy) +
                         ", per-iter-nvm-penalty=" + std::to_string(result.perIterNvmPenalty) +
                         ", restore-livein-margin=" + std::to_string(result.restoreLiveInMargin) +
                         ", commit-def-margin=" + std::to_string(result.commitDefMargin) +
                         ", boundary-state-margin=" + std::to_string(result.boundaryStateMargin) +
                         ", budget-after-boundary=" + std::to_string(result.budgetAfterBoundary);
        return result;
    }

    const double perIterTotalEnergy = result.pathEnergy + result.perIterNvmPenalty;
    if (perIterTotalEnergy <= 0.0) {
        result.reason = "nonpositive-per-iter-total-energy";
        result.details = "path-energy=" + std::to_string(result.pathEnergy) +
                         ", per-iter-nvm-penalty=" + std::to_string(result.perIterNvmPenalty) +
                         ", per-iter-total-energy=" + std::to_string(perIterTotalEnergy);
        return result;
    }

    const double strictBudget =
        std::nextafter(result.budgetAfterBoundary, -std::numeric_limits<double>::infinity());
    const double rawK = std::floor(strictBudget / perIterTotalEnergy);
    if (!std::isfinite(rawK) || rawK <= 0.0) {
        result.reason = "loop-total-exceeds-budget";
        result.details = "path-energy=" + std::to_string(result.pathEnergy) +
                         ", per-iter-path-energy=" + std::to_string(result.pathEnergy) +
                         ", per-iter-nvm-penalty=" + std::to_string(result.perIterNvmPenalty) +
                         ", restore-livein-margin=" + std::to_string(result.restoreLiveInMargin) +
                         ", commit-def-margin=" + std::to_string(result.commitDefMargin) +
                         ", boundary-state-margin=" + std::to_string(result.boundaryStateMargin) +
                         ", budget-after-boundary=" + std::to_string(result.budgetAfterBoundary) +
                         ", max-k=0";
        return result;
    }

    uint64_t maxK = static_cast<uint64_t>(rawK);
    if (maxK > static_cast<uint64_t>(std::numeric_limits<unsigned>::max()))
        maxK = std::numeric_limits<unsigned>::max();

    result.ok = true;
    result.maxK = maxK;
    return result;
}

} // namespace checkpoint
