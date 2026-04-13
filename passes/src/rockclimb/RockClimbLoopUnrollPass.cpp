#include "rockclimb/RockClimbLoopUnrollPass.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/LoopUtils.h"
#include "common/RockClimbConfig.h"
#include "estimator/EnergyEstimatorFactory.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

extern cl::opt<std::string> EnergyConfigOpt;

namespace {

cl::opt<std::string> RockClimbConfigOpt("rockclimb-config",
                                        cl::desc("Path to RockClimb config JSON"),
                                        cl::value_desc("filename"), cl::init(""));

enum class VisitState {
    Unvisited = 0,
    Visiting,
    Visited,
};

struct WorstCasePathResult {
    bool ok = false;
    double energy = 0.0;
    SmallPtrSet<const BasicBlock *, 16> blocksOnPath;
    std::string error;
};

struct LoopUnrollPlan {
    Loop *L = nullptr;
    uint64_t tripCount = 0;
    uint64_t unrollCount = 0;
    double iterEnergy = 0.0;
    double loopEnergy = 0.0;
};

struct PlanResult {
    std::optional<LoopUnrollPlan> plan;
    std::string skipReason;
};

struct SelectedLoopPlan {
    WeakTrackingVH headerHandle;
    LoopUnrollPlan plan;
};

static std::string getLoopHeaderName(const Loop *L) {
    if (!L) {
        return "<unknown>";
    }
    const BasicBlock *Header = L->getHeader();
    const Function *F = Header ? Header->getParent() : nullptr;
    if (!Header || !F) {
        return "<unknown>";
    }
    return checkpoint::getBlockName(*Header, *F);
}

static std::optional<uint64_t> getExactConstantTripCount(Loop *L, ScalarEvolution &SE) {
    if (!L) {
        return std::nullopt;
    }

    SmallVector<BasicBlock *, 4> exitingBlocks;
    L->getExitingBlocks(exitingBlocks);
    if (exitingBlocks.size() != 1) {
        return std::nullopt;
    }

    const SCEV *backedgeCount = SE.getBackedgeTakenCount(L);
    auto *backedgeConst = dyn_cast<SCEVConstant>(backedgeCount);
    if (!backedgeConst) {
        return std::nullopt;
    }

    const APInt &value = backedgeConst->getAPInt();
    if (value.getActiveBits() > 64) {
        return std::nullopt;
    }

    uint64_t backedgeValue = value.getZExtValue();
    bool exitAtLatch = (exitingBlocks.front() == L->getLoopLatch());
    if (exitAtLatch) {
        if (backedgeValue == std::numeric_limits<uint64_t>::max()) {
            return std::nullopt;
        }
        return backedgeValue + 1;
    }
    return backedgeValue;
}

static WorstCasePathResult
computeWorstCaseIterationEnergy(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
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
        std::optional<uint64_t> subTripCount = getExactConstantTripCount(SubL, SE);
        if (!subTripCount) {
            result.error = "sub-loop-unknown-trip-count";
            return result;
        }

        WorstCasePathResult subIter = computeWorstCaseIterationEnergy(SubL, blockEnergy, LI, SE);
        if (!subIter.ok) {
            result.error = subIter.error.empty() ? "sub-loop-energy-unavailable" : subIter.error;
            return result;
        }

        subLoopTotal[SubL] = subIter.energy * static_cast<double>(*subTripCount);
        for (const BasicBlock *BB : SubL->blocks()) {
            subLoopBlocks[SubL].insert(BB);
        }
    }

    auto getEnergy = [&](const BasicBlock *BB) -> double {
        Loop *ChildL = checkpoint::getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            auto it = subLoopTotal.find(ChildL);
            return (it != subLoopTotal.end()) ? it->second : 0.0;
        }
        auto it = blockEnergy.find(BB);
        return (it != blockEnergy.end()) ? it->second : 0.0;
    };

    auto getSuccs = [&](const BasicBlock *BB) -> SmallVector<const BasicBlock *, 4> {
        SmallVector<const BasicBlock *, 4> succs;
        Loop *ChildL = checkpoint::getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            SmallVector<BasicBlock *, 4> exits;
            ChildL->getExitBlocks(exits);
            for (BasicBlock *Exit : exits) {
                succs.push_back(Exit);
            }
        } else {
            for (const BasicBlock *Succ : successors(BB)) {
                succs.push_back(Succ);
            }
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
        result.error = "unable-to-compute-iteration-path";
        return result;
    }

    const BasicBlock *current = Header;
    while (current) {
        result.blocksOnPath.insert(current);
        Loop *ChildL = checkpoint::getDirectChildLoop(L, current, LI);
        if (ChildL && ChildL->getHeader() == current) {
            auto it = subLoopBlocks.find(ChildL);
            if (it != subLoopBlocks.end()) {
                result.blocksOnPath.insert(it->second.begin(), it->second.end());
            }
        }
        if (current == Latch) {
            break;
        }
        auto it = bestSucc.find(current);
        if (it == bestSucc.end()) {
            result.error = "path-reconstruction-failed";
            result.blocksOnPath.clear();
            return result;
        }
        current = it->second;
    }

    if (result.blocksOnPath.empty() || !result.blocksOnPath.count(Latch)) {
        result.error = "path-reconstruction-failed";
        result.blocksOnPath.clear();
        return result;
    }

    result.ok = true;
    result.energy = energy;
    return result;
}

static PlanResult computePlan(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
                              checkpoint::RockClimbParams params, LoopInfo &LI,
                              ScalarEvolution &SE) {
    PlanResult result;

    if (!L->isLoopSimplifyForm()) {
        result.skipReason = "not-loop-simplify-form";
        return result;
    }
    if (!L->hasDedicatedExits()) {
        result.skipReason = "no-dedicated-exits";
        return result;
    }
    if (!L->getLoopPreheader()) {
        result.skipReason = "missing-preheader";
        return result;
    }
    if (!L->getLoopLatch()) {
        result.skipReason = "missing-single-latch";
        return result;
    }
    if (checkpoint::containsInvoke(L)) {
        result.skipReason = "contains-invoke";
        return result;
    }

    std::optional<uint64_t> tripCount = getExactConstantTripCount(L, SE);
    if (!tripCount) {
        result.skipReason = "unknown-trip-count";
        return result;
    }
    if (*tripCount <= 1) {
        result.skipReason = "trip-count-not-beneficial";
        return result;
    }

    WorstCasePathResult iterEnergy = computeWorstCaseIterationEnergy(L, blockEnergy, LI, SE);
    if (!iterEnergy.ok || iterEnergy.energy <= 0.0) {
        result.skipReason = iterEnergy.error.empty() ? "iter-energy-unavailable" : iterEnergy.error;
        return result;
    }

    double ESafe = params.calculateESafe();
    if (ESafe <= 0.0) {
        result.skipReason = "nonpositive-energy-budget";
        return result;
    }

    double loopEnergy = static_cast<double>(*tripCount) * iterEnergy.energy;
    if (loopEnergy < ESafe) {
        result.skipReason = "loop-fits-budget";
        return result;
    }

    double strictBudget = std::nextafter(ESafe, -std::numeric_limits<double>::infinity());
    double rawK = std::floor(strictBudget / iterEnergy.energy);
    if (!std::isfinite(rawK) || rawK <= 1.0) {
        result.skipReason = "k-not-beneficial";
        return result;
    }

    uint64_t K = static_cast<uint64_t>(rawK);
    K = std::min<uint64_t>(K, 16);
    K = std::min<uint64_t>(K, *tripCount - 1);
    if (K <= 1) {
        result.skipReason = "k-not-beneficial";
        return result;
    }

    LoopUnrollPlan plan;
    plan.L = L;
    plan.tripCount = *tripCount;
    plan.unrollCount = K;
    plan.iterEnergy = iterEnergy.energy;
    plan.loopEnergy = loopEnergy;
    result.plan = plan;
    return result;
}

static void selectInNest(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
                         checkpoint::RockClimbParams params, LoopInfo &LI, ScalarEvolution &SE,
                         std::vector<SelectedLoopPlan> &out) {
    PlanResult planResult = computePlan(L, blockEnergy, params, LI, SE);
    if (planResult.plan) {
        out.push_back({WeakTrackingVH(L->getHeader()), *planResult.plan});
        return;
    }

    for (Loop *SubL : L->getSubLoops()) {
        selectInNest(SubL, blockEnergy, params, LI, SE, out);
    }
}

static std::vector<SelectedLoopPlan>
selectLoopsToUnroll(LoopInfo &LI, const DenseMap<const BasicBlock *, double> &blockEnergy,
                    checkpoint::RockClimbParams params, ScalarEvolution &SE) {
    std::vector<SelectedLoopPlan> selected;
    for (Loop *L : LI) {
        selectInNest(L, blockEnergy, params, LI, SE, selected);
    }
    return selected;
}

static bool applyUnrollPlan(const LoopUnrollPlan &plan, LoopInfo &LI, ScalarEvolution &SE,
                            DominatorTree &DT, AssumptionCache &AC, AAResults &AA,
                            const TargetTransformInfo &TTI, OptimizationRemarkEmitter &ORE) {
    UnrollLoopOptions options{};
    options.Count = static_cast<unsigned>(plan.unrollCount);
    options.Force = true;
    options.Runtime = false;
    options.AllowExpensiveTripCount = false;
    options.UnrollRemainder = true;
    options.ForgetAllSCEV = false;
    options.SCEVExpansionBudget = SCEVCheapExpansionBudget;
    options.RuntimeUnrollMultiExit = false;
    options.AddAdditionalAccumulators = false;

    Loop *RemainderLoop = nullptr;
    LoopUnrollResult result = UnrollLoop(plan.L, options, &LI, &SE, &DT, &AC, &TTI, &ORE,
                                         /*PreserveLCSSA=*/true, &RemainderLoop, &AA);
    return result != LoopUnrollResult::Unmodified;
}

} // namespace

namespace checkpoint {

PreservedAnalyses RockClimbLoopUnrollPass::run(Function &F, FunctionAnalysisManager &AM) {
    checkpoint::initLogging();

    if (F.isDeclaration() || F.empty()) {
        return PreservedAnalyses::all();
    }

    auto &LI = AM.getResult<LoopAnalysis>(F);
    if (LI.empty()) {
        return PreservedAnalyses::all();
    }

    if (RockClimbConfigOpt.getValue().empty()) {
        PLOGE << "RockClimbLoopUnrollPass: missing -rockclimb-config; skipping " << F.getName();
        return PreservedAnalyses::all();
    }
    if (EnergyConfigOpt.getValue().empty()) {
        PLOGE << "RockClimbLoopUnrollPass: missing -energy-config; skipping " << F.getName();
        return PreservedAnalyses::all();
    }

    RockClimbParams params;
    if (!parseRockClimbParams(RockClimbConfigOpt.getValue(), params)) {
        PLOGE << "RockClimbLoopUnrollPass: failed to parse config; skipping " << F.getName();
        return PreservedAnalyses::all();
    }

    auto factory = EnergyEstimatorFactory::createDefault();
    std::unique_ptr<EnergyEstimator> estimator =
        factory.createFromConfig(EnergyConfigOpt.getValue());
    if (!estimator) {
        PLOGE << "RockClimbLoopUnrollPass: failed to create estimator for " << F.getName();
        return PreservedAnalyses::all();
    }
    estimator->prepareForFunction(F);

    DenseMap<const BasicBlock *, double> blockEnergy;
    blockEnergy.reserve(F.size());
    for (BasicBlock &BB : F) {
        blockEnergy[&BB] = estimator->estimate(BB).cost;
    }

    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);
    auto &AC = AM.getResult<AssumptionAnalysis>(F);
    auto &TTI = AM.getResult<TargetIRAnalysis>(F);
    auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);

    auto selected = selectLoopsToUnroll(LI, blockEnergy, params, SE);
    if (selected.empty()) {
        estimator->finalizeFunction(F);
        return PreservedAnalyses::all();
    }

    bool changed = false;
    for (auto &item : selected) {
        auto *Header = dyn_cast_or_null<BasicBlock>(item.headerHandle);
        if (!Header) {
            continue;
        }

        Loop *L = LI.getLoopFor(Header);
        if (!L || L->getHeader() != Header) {
            continue;
        }

        item.plan.L = L;
        if (!applyUnrollPlan(item.plan, LI, SE, DT, AC, AA, TTI, ORE)) {
            PLOGW << "RockClimbLoopUnrollPass: failed to unroll " << F.getName()
                  << "::" << getLoopHeaderName(L) << " K=" << item.plan.unrollCount;
            continue;
        }

        changed = true;
        PLOGI << "RockClimbLoopUnrollPass: unrolled " << F.getName() << "::" << getLoopHeaderName(L)
              << " N=" << item.plan.tripCount << " K=" << item.plan.unrollCount
              << " E_iter_wc=" << item.plan.iterEnergy << " E_loop_wc=" << item.plan.loopEnergy
              << " E_safe=" << params.calculateESafe();
    }

    estimator->finalizeFunction(F);
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace checkpoint
