#include "milp/LoopStripMiningPass.h"

#include "common/AnnotationUtils.h"
#include "common/BlockUtils.h"
#include "common/LoopTripCount.h"
#include "common/LoopUtils.h"
#include "estimator/EnergyEstimatorFactory.h"
#include "milp/EnergyModel.h"
#include "milp/EnergyPathUtils.h"
#include "milp/StateAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;
extern cl::opt<bool> LoopStripMiningEnabledOpt;

namespace {

static cl::opt<bool> LoopStripMiningVerboseOpt(
    "loop-strip-mining-verbose",
    cl::desc("Print per-loop strip-mining decisions"),
    cl::init(false));

struct LoopRewritePlan {
    Loop *L = nullptr;
    uint64_t N = 0;
    uint64_t K = 0;
    double iterEnergy = 0.0;
    bool isChunking = false;
};

struct PlanResult {
    std::optional<LoopRewritePlan> plan;
    std::string skipReason;
    std::string skipDetail;
};

using checkpoint::WorstCasePathResult;

enum class VisitState {
    Unvisited = 0,
    Visiting,
    Visited,
};

struct LoopStripMiningStats {
    unsigned loopsSeen = 0;
    unsigned loopsEligible = 0;
    unsigned loopsRewritten = 0;
    unsigned loopsChunked = 0;
    std::map<std::string, unsigned> skippedReasons;
    std::vector<std::pair<std::string, uint64_t>> chosenKByHeader;
};

struct HeaderPhiInfo {
    PHINode *headerPhi;
    PHINode *outerPhi;
    Value   *initVal;
};

using checkpoint::containsInvoke;
using checkpoint::getDirectChildLoop;
using checkpoint::getMarkerTripCount;
using checkpoint::removeLoopTripCountMetadata;
using checkpoint::removeStripMinedLoopMetadata;
using checkpoint::setLoopTripCountMetadata;
using checkpoint::setStripMinedLoopMetadata;

static std::optional<uint64_t> getConstantTripCount(Loop *L,
                                                    ScalarEvolution &SE,
                                                    const BasicBlock *ExitingBlock);

static WorstCasePathResult computeWorstCaseIterationEnergy(
    Loop *L,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    LoopInfo &LI,
    ScalarEvolution &SE) {
    WorstCasePathResult result;

    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    if (!Header || !Latch) {
        result.error = "missing-header-or-latch";
        return result;
    }

    // Step 1: Recursively compute total energy for each direct sub-loop.
    DenseMap<const Loop *, double> subLoopTotal;
    std::map<const Loop *, SmallPtrSet<const BasicBlock *, 16>> subLoopBlocks;
    for (Loop *SubL : L->getSubLoops()) {
        std::optional<uint64_t> scevTC;
        SmallVector<BasicBlock *, 8> subExiting;
        SubL->getExitingBlocks(subExiting);
        if (subExiting.size() == 1)
            scevTC = getConstantTripCount(SubL, SE, subExiting.front());
        auto markerTC = getMarkerTripCount(SubL);

        std::optional<uint64_t> subTC;
        if (scevTC && markerTC)
            subTC = std::min(*scevTC, *markerTC);
        else if (scevTC)
            subTC = scevTC;
        else
            subTC = markerTC;

        if (!subTC) {
            result.error = "sub-loop-unknown-trip-count";
            return result;
        }

        auto subIter = computeWorstCaseIterationEnergy(SubL, blockEnergy, LI, SE);
        if (!subIter.ok) {
            result.error = "sub-loop-energy-unavailable";
            return result;
        }

        subLoopTotal[SubL] = subIter.energy * static_cast<double>(*subTC);
        for (const BasicBlock *BB : SubL->blocks())
            subLoopBlocks[SubL].insert(BB);
    }

    // Step 2: Block energy with sub-loop collapsing.
    auto getEnergy = [&](const BasicBlock *BB) -> double {
        Loop *ChildL = getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            auto it = subLoopTotal.find(ChildL);
            return (it != subLoopTotal.end()) ? it->second : 0.0;
        }
        auto it = blockEnergy.find(BB);
        return (it != blockEnergy.end()) ? it->second : 0.0;
    };

    // Step 3: Successor computation with sub-loop collapsing.
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

    std::function<double(const BasicBlock *)> dfs =
        [&](const BasicBlock *BB) -> double {
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

    SmallPtrSet<const BasicBlock *, 16> pathBlocks;
    const BasicBlock *cur = Header;
    while (cur) {
        pathBlocks.insert(cur);
        // If this block is an inner-loop header, expand with all sub-loop blocks
        // so state margins are aggregated consistently with AbstractCFG.
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

static std::optional<uint64_t> getConstantTripCount(Loop *L,
                                                    ScalarEvolution &SE,
                                                    const BasicBlock *ExitingBlock) {
    const SCEV *btc = SE.getBackedgeTakenCount(L);
    auto *btcConst = dyn_cast<SCEVConstant>(btc);
    if (!btcConst) {
        return std::nullopt;
    }

    const APInt &backedgeCount = btcConst->getAPInt();
    const BasicBlock *Header = L->getHeader();
    const Function *F = Header ? Header->getParent() : nullptr;
    std::string loopId = "<unknown>";
    if (Header && F) {
        loopId = (F->getName() + "::" +
                  checkpoint::getBlockName(*Header, *F))
                     .str();
    }

    if (backedgeCount.getActiveBits() > 64) {
        errs() << "LoopStripMiningPass warning: backedge count for loop "
               << loopId << " exceeds 64 bits; skipping loop\n";
        return std::nullopt;
    }

    uint64_t backedgeValue = backedgeCount.getZExtValue();
    bool exitAtLatch = (ExitingBlock == L->getLoopLatch());
    if (exitAtLatch &&
        backedgeValue == std::numeric_limits<uint64_t>::max()) {
        errs() << "LoopStripMiningPass warning: backedge count for loop "
               << loopId << " cannot be incremented safely; skipping loop\n";
        return std::nullopt;
    }

    // For loops exiting at the header, backedge count matches loop-body
    // iterations. For loops exiting at the latch, body iterations are one more.
    if (exitAtLatch) {
        return backedgeValue + 1;
    }
    return backedgeValue;
}

using checkpoint::computeBoundaryStateMarginOnPath;

static double computeNvmAccessMarginOnPath(
    const SmallPtrSetImpl<const BasicBlock *> &pathBlocks,
    const checkpoint::StateAnalysis &state,
    const checkpoint::MILPEnergyParams &params) {
    std::vector<GlobalVariable *> ineligGlobals;
    for (Value *V : state.getIneligibleObjs()) {
        if (auto *GV = dyn_cast<GlobalVariable>(V))
            ineligGlobals.push_back(GV);
    }

    double nvmAccessMargin = 0.0;
    for (const BasicBlock *BB : pathBlocks) {
        for (GlobalVariable *GV : state.getVMObjs()) {
            unsigned accesses =
                state.getLoadCount(BB, GV) + state.getStoreCount(BB, GV);
            nvmAccessMargin +=
                static_cast<double>(accesses) * params.nvmAccessPenalty;
        }
        for (GlobalVariable *GV : ineligGlobals) {
            unsigned accesses =
                state.getLoadCount(BB, GV) + state.getStoreCount(BB, GV);
            nvmAccessMargin +=
                static_cast<double>(accesses) * params.nvmAccessPenalty;
        }
    }
    return nvmAccessMargin;
}

static PlanResult buildRewritePlan(
    Loop *L,
    ScalarEvolution &SE,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params,
    LoopInfo &LI,
    const checkpoint::StateAnalysis &state) {
    PlanResult result;
    result.skipReason = "unknown";

    // ── Common checks (both strip mining and chunking) ──
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
    BasicBlock *Latch = L->getLoopLatch();
    if (!Latch) {
        result.skipReason = "missing-single-latch";
        return result;
    }
    if (containsInvoke(L)) {
        result.skipReason = "contains-invoke";
        return result;
    }

    // ── Common: compute energy budget and K ──
    double budget = params.capacity - params.E_pro - params.E_epi;
    if (budget <= 0.0) {
        result.skipReason = "nonpositive-energy-budget";
        result.skipDetail = "capacity=" + std::to_string(params.capacity) +
            ", E_pro=" + std::to_string(params.E_pro) +
            ", E_epi=" + std::to_string(params.E_epi) +
            ", budget=" + std::to_string(budget);
        return result;
    }

    WorstCasePathResult iterEnergy =
        computeWorstCaseIterationEnergy(L, blockEnergy, LI, SE);
    if (!iterEnergy.ok) {
        result.skipReason = iterEnergy.error;
        return result;
    }
    if (iterEnergy.energy <= 0.0) {
        result.skipReason = "nonpositive-iteration-energy";
        return result;
    }

    double perIterNvmPenalty = computeNvmAccessMarginOnPath(
        iterEnergy.blocksOnPath, state, params);
    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = computeBoundaryStateMarginOnPath(
        iterEnergy.blocksOnPath,
        state,
        params,
        restoreLiveInMargin,
        commitDefMargin);
    double budgetAfterBoundary = budget - boundaryStateMargin;
    if (budgetAfterBoundary <= 0.0) {
        result.skipReason = "nonpositive-effective-budget";
        result.skipDetail = "budget=" + std::to_string(budget) +
            ", per-iter-nvm-penalty=" + std::to_string(perIterNvmPenalty) +
            ", per-iter-path-energy=" + std::to_string(iterEnergy.energy) +
            ", restore-livein-margin=" + std::to_string(restoreLiveInMargin) +
            ", commit-def-margin=" + std::to_string(commitDefMargin) +
            ", boundary-state-margin=" + std::to_string(boundaryStateMargin) +
            ", budget-after-boundary=" + std::to_string(budgetAfterBoundary);
        return result;
    }

    double perIterTotalEnergy = iterEnergy.energy + perIterNvmPenalty;
    if (perIterTotalEnergy <= 0.0) {
        result.skipReason = "nonpositive-per-iter-total-energy";
        result.skipDetail =
            "per-iter-path-energy=" + std::to_string(iterEnergy.energy) +
            ", per-iter-nvm-penalty=" + std::to_string(perIterNvmPenalty) +
            ", per-iter-total-energy=" + std::to_string(perIterTotalEnergy);
        return result;
    }

    // Enforce strict inequality: K * perIterTotalEnergy < budgetAfterBoundary.
    double strictBudget =
        std::nextafter(budgetAfterBoundary, -std::numeric_limits<double>::infinity());
    double rawK = std::floor(strictBudget / perIterTotalEnergy);
    if (!std::isfinite(rawK) || rawK <= 0.0) {
        result.skipReason = "k-zero";
        result.skipDetail =
            "budget-after-boundary=" + std::to_string(budgetAfterBoundary) +
            ", strict-budget=" + std::to_string(strictBudget) +
            ", per-iter-path-energy=" + std::to_string(iterEnergy.energy) +
            ", per-iter-nvm-penalty=" + std::to_string(perIterNvmPenalty) +
            ", per-iter-total-energy=" + std::to_string(perIterTotalEnergy);
        return result;
    }

    uint64_t K = static_cast<uint64_t>(rawK);
    if (K <= 1) {
        result.skipReason = "k-not-beneficial";
        return result;
    }
    if (K > std::numeric_limits<unsigned>::max()) {
        result.skipReason = "k-too-large";
        return result;
    }

    // ── Tier 1: Try strip mining ──
    // Requires: canonical IV, single exiting block, constant trip count, K < N
    PHINode *IV = L->getCanonicalInductionVariable();
    SmallVector<BasicBlock *, 8> exitingBlocks;
    L->getExitingBlocks(exitingBlocks);

    if (IV && exitingBlocks.size() == 1) {
        std::optional<uint64_t> tripCount =
            getConstantTripCount(L, SE, exitingBlocks.front());
        if (tripCount && *tripCount >= 2) {
            if (K >= *tripCount) {
                result.skipReason = "k-covers-entire-loop";
                return result;
            }
            LoopRewritePlan plan;
            plan.L = L;
            plan.N = *tripCount;
            plan.K = K;
            plan.iterEnergy = iterEnergy.energy;
            plan.isChunking = false;
            result.plan = plan;
            result.skipReason.clear();
            return result;
        }
    }

    // ── Tier 2: Chunking fallback ──
    // Only needs: latch has a BranchInst with backedge to header
    BasicBlock *Header = L->getHeader();
    BranchInst *LatchBr = dyn_cast<BranchInst>(Latch->getTerminator());
    if (!LatchBr) {
        result.skipReason = "latch-not-branch-inst";
        return result;
    }

    bool hasBackedge = false;
    for (unsigned i = 0; i < LatchBr->getNumSuccessors(); i++) {
        if (LatchBr->getSuccessor(i) == Header) {
            hasBackedge = true;
            break;
        }
    }
    if (!hasBackedge) {
        result.skipReason = "latch-no-backedge-to-header";
        return result;
    }

    // If we can determine trip count and K covers it, skip
    std::optional<uint64_t> knownTC;
    if (exitingBlocks.size() == 1)
        knownTC = getConstantTripCount(L, SE, exitingBlocks.front());
    if (!knownTC)
        knownTC = getMarkerTripCount(L);
    if (knownTC && K >= *knownTC) {
        result.skipReason = "k-covers-entire-loop";
        return result;
    }

    LoopRewritePlan plan;
    plan.L = L;
    // Carry known upper-bound trip count into chunking so the generated
    // outer loop can be annotated with a conservative upper bound.
    plan.N = knownTC ? *knownTC : 0;
    plan.K = K;
    plan.iterEnergy = iterEnergy.energy;
    plan.isChunking = true;
    result.plan = plan;
    result.skipReason.clear();
    return result;
}

static void refreshBlockEnergy(Function &F,
                               checkpoint::EnergyEstimator &estimator,
                               DenseMap<const BasicBlock *, double> &blockEnergy) {
    estimator.prepareForFunction(F);
    blockEnergy.clear();
    blockEnergy.reserve(F.size());
    for (BasicBlock &BB : F) {
        blockEnergy[&BB] = estimator.estimate(BB).cost;
    }
}

struct ChunkBudgetResult {
    bool ok = false;
    uint64_t maxK = 0;
    double iterEnergy = 0.0;
    std::string error;
};

static ChunkBudgetResult recomputeChunkKWithOverhead(
    Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params, LoopInfo &LI,
    ScalarEvolution &SE, const checkpoint::StateAnalysis &state) {
    ChunkBudgetResult out;

    double budget = params.capacity - params.E_pro - params.E_epi;
    if (budget <= 0.0) {
        out.error = "nonpositive-energy-budget";
        return out;
    }

    WorstCasePathResult iterEnergy =
        computeWorstCaseIterationEnergy(L, blockEnergy, LI, SE);
    if (!iterEnergy.ok || iterEnergy.energy <= 0.0) {
        out.error = iterEnergy.error.empty() ? "post-chunk-iter-energy-unavailable"
                                             : iterEnergy.error;
        return out;
    }

    double perIterNvmPenalty = computeNvmAccessMarginOnPath(
        iterEnergy.blocksOnPath, state, params);
    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = computeBoundaryStateMarginOnPath(
        iterEnergy.blocksOnPath,
        state,
        params,
        restoreLiveInMargin,
        commitDefMargin);
    double budgetAfterBoundary = budget - boundaryStateMargin;
    if (budgetAfterBoundary <= 0.0) {
        out.error = "nonpositive-effective-budget";
        return out;
    }

    double perIterTotalEnergy = iterEnergy.energy + perIterNvmPenalty;
    if (perIterTotalEnergy <= 0.0) {
        out.error = "nonpositive-per-iter-total-energy";
        return out;
    }

    // Enforce strict inequality: K * perIterTotalEnergy < budgetAfterBoundary.
    double strictBudget =
        std::nextafter(budgetAfterBoundary, -std::numeric_limits<double>::infinity());
    double rawK = std::floor(strictBudget / perIterTotalEnergy);
    if (!std::isfinite(rawK) || rawK <= 0.0) {
        out.error = "post-chunk-k-zero";
        return out;
    }

    uint64_t maxK = static_cast<uint64_t>(rawK);
    if (maxK > std::numeric_limits<unsigned>::max()) {
        maxK = std::numeric_limits<unsigned>::max();
    }

    out.ok = true;
    out.maxK = maxK;
    out.iterEnergy = iterEnergy.energy;
    return out;
}

static bool updateChunkLoopBound(Loop *L, uint64_t newK) {
    BasicBlock *Header = L->getHeader();
    if (!Header) {
        return false;
    }

    PHINode *ChunkCounter = nullptr;
    for (PHINode &PN : Header->phis()) {
        if (PN.getName().starts_with("chunk.counter")) {
            ChunkCounter = &PN;
            break;
        }
    }
    if (!ChunkCounter) {
        return false;
    }

    BasicBlock *CounterCheck = nullptr;
    for (unsigned i = 0; i < ChunkCounter->getNumIncomingValues(); i++) {
        BasicBlock *InBB = ChunkCounter->getIncomingBlock(i);
        if (InBB && L->contains(InBB)) {
            CounterCheck = InBB;
            break;
        }
    }
    if (!CounterCheck) {
        return false;
    }

    auto *Br = dyn_cast<BranchInst>(CounterCheck->getTerminator());
    if (!Br || !Br->isConditional()) {
        return false;
    }

    auto *Cmp = dyn_cast<ICmpInst>(Br->getCondition());
    if (!Cmp) {
        return false;
    }

    auto *IntTy = dyn_cast<IntegerType>(ChunkCounter->getType());
    if (!IntTy) {
        return false;
    }
    ConstantInt *Bound = ConstantInt::get(IntTy, newK);

    if (isa<ConstantInt>(Cmp->getOperand(0))) {
        Cmp->setOperand(0, Bound);
        return true;
    }
    if (isa<ConstantInt>(Cmp->getOperand(1))) {
        Cmp->setOperand(1, Bound);
        return true;
    }

    return false;
}

static void selectInNest(
    Loop *L, ScalarEvolution &SE,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params, LoopInfo &LI,
    const checkpoint::StateAnalysis &state,
    std::vector<std::pair<Loop *, LoopRewritePlan>> &out,
    LoopStripMiningStats &stats) {

    stats.loopsSeen++;
    PlanResult pr = buildRewritePlan(L, SE, blockEnergy, params, LI, state);

    if (pr.plan) {
        out.emplace_back(L, *pr.plan);
        return;
    }

    if (LoopStripMiningVerboseOpt) {
        BasicBlock *Header = L->getHeader();
        const Function *F = Header ? Header->getParent() : nullptr;
        std::string headerName = (Header && F)
            ? checkpoint::getBlockName(*Header, *F) : "<unknown>";
        std::string funcName = F ? F->getName().str() : "<unknown>";
        errs() << "LoopStripMiningPass: skip " << funcName << "::"
               << headerName << " reason=" << pr.skipReason;
        if (!pr.skipDetail.empty()) {
            errs() << " " << pr.skipDetail;
        }
        errs() << "\n";
    }

    if (pr.skipReason == "k-covers-entire-loop") {
        stats.skippedReasons[pr.skipReason]++;
        return;
    }

    stats.skippedReasons[pr.skipReason]++;
    for (Loop *SubL : L->getSubLoops()) {
        selectInNest(SubL, SE, blockEnergy, params, LI, state, out, stats);
    }
}

static std::vector<std::pair<Loop *, LoopRewritePlan>> selectLoopsToStripMine(
    LoopInfo &LI, ScalarEvolution &SE,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params,
    const checkpoint::StateAnalysis &state,
    LoopStripMiningStats &stats) {
    std::vector<std::pair<Loop *, LoopRewritePlan>> selected;
    for (Loop *L : LI) {
        selectInNest(L, SE, blockEnergy, params, LI, state, selected, stats);
    }
    return selected;
}

static bool stripMineLoop(const LoopRewritePlan &plan,
                          LoopInfo &LI,
                          ScalarEvolution &SE,
                          DominatorTree &DT,
                          AssumptionCache &AC,
                          AAResults &AA,
                          const TargetTransformInfo &TTI) {
    // ── Phase 1: Extract and validate loop components ──
    Loop *L = plan.L;
    uint64_t N = plan.N, K = plan.K;

    BasicBlock *Preheader = L->getLoopPreheader();
    BasicBlock *Header    = L->getHeader();
    BasicBlock *Latch     = L->getLoopLatch();
    BasicBlock *ExitBlock = L->getExitBlock();
    BasicBlock *ExitingBB = L->getExitingBlock();
    Function *F = Header->getParent();
    LLVMContext &Ctx = F->getContext();

    if (!Preheader || !Header || !Latch || !ExitBlock || !ExitingBB)
        return false;

    PHINode *IV = L->getCanonicalInductionVariable();
    if (!IV) return false;
    Type *IVTy = IV->getType();

    // ── Phase 2: Find exit condition ──
    BranchInst *ExitBr = dyn_cast<BranchInst>(ExitingBB->getTerminator());
    if (!ExitBr || !ExitBr->isConditional()) return false;

    ICmpInst *ExitCmp = dyn_cast<ICmpInst>(ExitBr->getCondition());
    if (!ExitCmp) return false;

    Value *CmpOp0 = ExitCmp->getOperand(0);
    Value *CmpOp1 = ExitCmp->getOperand(1);
    BasicBlock *BackedgeBB = nullptr, *IncomingBB = nullptr;
    if (!L->getIncomingAndBackEdge(IncomingBB, BackedgeBB)) return false;
    Value *IVNext = IV->getIncomingValueForBlock(BackedgeBB);

    int boundOperandIdx = -1;
    if (CmpOp0 == IV || CmpOp0 == IVNext) boundOperandIdx = 1;
    else if (CmpOp1 == IV || CmpOp1 == IVNext) boundOperandIdx = 0;
    else return false;

    auto *OrigBound = dyn_cast<ConstantInt>(ExitCmp->getOperand(boundOperandIdx));
    if (!OrigBound || OrigBound->getZExtValue() != N) return false;

    // Collect LCSSA PHIs in ExitBlock before any modifications
    SmallVector<PHINode *, 4> lcssaPhis;
    for (PHINode &PN : ExitBlock->phis())
        lcssaPhis.push_back(&PN);

    // ── Phase 3: Create outer loop blocks ──
    BasicBlock *OuterHeader = BasicBlock::Create(Ctx, "outer.header", F, Header);
    BasicBlock *OuterLatch  = BasicBlock::Create(Ctx, "outer.latch", F, ExitBlock);

    // ── Phase 4: Build OuterHeader ──
    IRBuilder<> OHB(OuterHeader);
    PHINode *OuterIV = OHB.CreatePHI(IVTy, 2, "outer.iv");

    // Forward non-IV Header PHIs through the outer loop
    SmallVector<HeaderPhiInfo, 4> headerPhiForwarding;
    for (PHINode &PN : Header->phis()) {
        if (&PN == IV) continue;
        Value *InitVal = PN.getIncomingValueForBlock(Preheader);
        PHINode *OHP = OHB.CreatePHI(PN.getType(), 2, PN.getName() + ".outer");
        headerPhiForwarding.push_back({&PN, OHP, InitVal});
    }

    // Inner limit: min(outer.iv + K, N)
    Value *OuterIVPlusK = OHB.CreateAdd(OuterIV, ConstantInt::get(IVTy, K),
                                        "outer.iv.plus.k");
    Value *NVal = ConstantInt::get(IVTy, N);
    Value *Cmp = OHB.CreateICmpULT(OuterIVPlusK, NVal, "min.cmp");
    Value *InnerLimit = OHB.CreateSelect(Cmp, OuterIVPlusK, NVal, "inner.limit");

    OHB.CreateBr(Header);

    // ── Phase 5: Rewire Preheader → OuterHeader + update Header PHIs ──
    Preheader->getTerminator()->replaceSuccessorWith(Header, OuterHeader);

    for (PHINode &PN : Header->phis()) {
        int idx = PN.getBasicBlockIndex(Preheader);
        if (idx >= 0) {
            PN.setIncomingBlock(idx, OuterHeader);
            if (&PN == IV) {
                PN.setIncomingValue(idx, OuterIV);
            } else {
                for (auto &info : headerPhiForwarding) {
                    if (info.headerPhi == &PN) {
                        PN.setIncomingValue(idx, info.outerPhi);
                        break;
                    }
                }
            }
        }
    }

    // ── Phase 6: Modify inner loop exit ──
    ExitCmp->setOperand(boundOperandIdx, InnerLimit);
    ExitBr->replaceSuccessorWith(ExitBlock, OuterLatch);

    // ── Phase 7: Build OuterLatch ──
    IRBuilder<> OLB(OuterLatch);

    // Forwarding PHIs for LCSSA values escaping to ExitBlock
    SmallVector<PHINode *, 4> outerLatchPhis;
    for (PHINode *LCPhi : lcssaPhis) {
        Value *IncomingVal = LCPhi->getIncomingValueForBlock(ExitingBB);
        PHINode *OLP = OLB.CreatePHI(LCPhi->getType(), 1,
                                     LCPhi->getName() + ".ol");
        OLP->addIncoming(IncomingVal, ExitingBB);
        outerLatchPhis.push_back(OLP);
    }

    // Forwarding PHIs for non-IV Header PHIs (loop-carried state)
    SmallVector<PHINode *, 4> headerForwardPhis;
    for (auto &info : headerPhiForwarding) {
        Value *ForwardVal;
        if (ExitingBB == Header) {
            ForwardVal = info.headerPhi;
        } else {
            ForwardVal = info.headerPhi->getIncomingValueForBlock(Latch);
        }
        PHINode *FP = OLB.CreatePHI(info.headerPhi->getType(), 1,
                                    info.headerPhi->getName() + ".fwd");
        FP->addIncoming(ForwardVal, ExitingBB);
        headerForwardPhis.push_back(FP);
    }

    // Next outer IV
    Value *NextOuterIV = OLB.CreateAdd(OuterIV, ConstantInt::get(IVTy, K),
                                       "outer.iv.next");

    // Outer exit condition: outer runs ceil(N/K) times
    uint64_t outerTripCount = (N + K - 1) / K;
    Value *OuterContinue = OLB.CreateICmpULT(NextOuterIV,
                                             ConstantInt::get(IVTy, N),
                                             "outer.cmp");
    OLB.CreateCondBr(OuterContinue, OuterHeader, ExitBlock);

    // Complete OuterIV PHI
    OuterIV->addIncoming(ConstantInt::get(IVTy, 0), Preheader);
    OuterIV->addIncoming(NextOuterIV, OuterLatch);

    // Complete non-IV OuterHeader PHIs
    for (unsigned i = 0; i < headerPhiForwarding.size(); i++) {
        auto &info = headerPhiForwarding[i];
        info.outerPhi->addIncoming(info.initVal, Preheader);
        info.outerPhi->addIncoming(headerForwardPhis[i], OuterLatch);
    }

    // ── Phase 8: Update ExitBlock PHIs ──
    for (unsigned i = 0; i < lcssaPhis.size(); i++) {
        int idx = lcssaPhis[i]->getBasicBlockIndex(ExitingBB);
        lcssaPhis[i]->setIncomingBlock(idx, OuterLatch);
        lcssaPhis[i]->setIncomingValue(idx, outerLatchPhis[i]);
    }

    // ── Phase 9: Update LoopInfo ──
    Loop *ParentLoop = L->getParentLoop();
    Loop *OuterLoop = LI.AllocateLoop();

    if (ParentLoop) {
        for (auto I = ParentLoop->begin(); I != ParentLoop->end(); ++I) {
            if (*I == L) { ParentLoop->removeChildLoop(I); break; }
        }
        ParentLoop->addChildLoop(OuterLoop);
    } else {
        for (auto I = LI.begin(); I != LI.end(); ++I) {
            if (*I == L) { LI.removeLoop(I); break; }
        }
        LI.addTopLevelLoop(OuterLoop);
    }
    OuterLoop->addChildLoop(L);

    OuterLoop->addBasicBlockToLoop(OuterHeader, LI);
    OuterLoop->addBasicBlockToLoop(OuterLatch, LI);

    for (BasicBlock *BB : L->blocks())
        OuterLoop->addBlockEntry(BB);

    OuterLoop->moveToHeader(OuterHeader);

    // ── Phase 10: Rebuild DominatorTree ──
    DT.recalculate(*F);

    // ── Phase 11: Tripcount markers, LCSSA repair, SCEV invalidation ──
    removeLoopTripCountMetadata(L);
    setLoopTripCountMetadata(L, K);
    // Mark the K-bounded inner loop as strip-mined; this is the loop that
    // should be summarized by AbstractCFG.
    setStripMinedLoopMetadata(L);

    setLoopTripCountMetadata(OuterLoop, outerTripCount);

    formLCSSARecursively(*OuterLoop, DT, &LI, &SE);

    SE.forgetLoop(L);

    return true;
}

static bool chunkLoop(const LoopRewritePlan &plan,
                      LoopInfo &LI,
                      ScalarEvolution &SE,
                      DominatorTree &DT) {
    // ── Phase 1: Extract loop components ──
    Loop *L = plan.L;
    uint64_t K = plan.K;
    uint64_t NUpper = plan.N;

    BasicBlock *Preheader = L->getLoopPreheader();
    BasicBlock *Header    = L->getHeader();
    BasicBlock *Latch     = L->getLoopLatch();
    Function *F = Header->getParent();
    LLVMContext &Ctx = F->getContext();

    if (!Preheader || !Header || !Latch)
        return false;

    // ── Phase 2: Find latch backedge index (which successor == Header) ──
    BranchInst *LatchBr = dyn_cast<BranchInst>(Latch->getTerminator());
    if (!LatchBr)
        return false;

    unsigned backedgeIdx = UINT_MAX;
    for (unsigned i = 0; i < LatchBr->getNumSuccessors(); i++) {
        if (LatchBr->getSuccessor(i) == Header) {
            backedgeIdx = i;
            break;
        }
    }
    if (backedgeIdx == UINT_MAX)
        return false;

    // ── Phase 3: Create outer.header, counter.check, outer.latch blocks ──
    BasicBlock *OuterHeader  = BasicBlock::Create(Ctx, "outer.header", F, Header);
    BasicBlock *CounterCheck = BasicBlock::Create(Ctx, "counter.check", F);
    BasicBlock *OuterLatch   = BasicBlock::Create(Ctx, "outer.latch", F);

    // ── Phase 4: Build outer.header — forward all header PHIs through outer PHIs ──
    IRBuilder<> OHB(OuterHeader);

    SmallVector<HeaderPhiInfo, 8> headerPhiForwarding;
    for (PHINode &PN : Header->phis()) {
        Value *InitVal = PN.getIncomingValueForBlock(Preheader);
        PHINode *OHP = OHB.CreatePHI(PN.getType(), 2, PN.getName() + ".outer");
        headerPhiForwarding.push_back({&PN, OHP, InitVal});
    }
    OHB.CreateBr(Header);

    // ── Phase 5: Rewire Preheader → OuterHeader, update header PHI incoming ──
    Preheader->getTerminator()->replaceSuccessorWith(Header, OuterHeader);

    for (auto &info : headerPhiForwarding) {
        int idx = info.headerPhi->getBasicBlockIndex(Preheader);
        if (idx >= 0) {
            info.headerPhi->setIncomingBlock(idx, OuterHeader);
            info.headerPhi->setIncomingValue(idx, info.outerPhi);
        }
    }

    // ── Phase 6: Add chunk.counter PHI to header ──
    Type *I64Ty = Type::getInt64Ty(Ctx);
    PHINode *ChunkCounter = PHINode::Create(I64Ty, 2, "chunk.counter",
                                            Header->getFirstNonPHIIt());

    // ── Phase 7: Redirect latch backedge: Latch → CounterCheck ──
    LatchBr->setSuccessor(backedgeIdx, CounterCheck);

    // ── Phase 8: Build counter.check — increment counter, branch ──
    IRBuilder<> CCB(CounterCheck);
    Value *CounterNext = CCB.CreateAdd(ChunkCounter,
                                       ConstantInt::get(I64Ty, 1),
                                       "counter.next");
    Value *CounterDone = CCB.CreateICmpEQ(CounterNext,
                                          ConstantInt::get(I64Ty, K),
                                          "counter.done");
    CCB.CreateCondBr(CounterDone, OuterLatch, Header);

    // ── Phase 9: Update header PHIs — incoming block Latch → CounterCheck ──
    for (auto &info : headerPhiForwarding) {
        int idx = info.headerPhi->getBasicBlockIndex(Latch);
        if (idx >= 0) {
            info.headerPhi->setIncomingBlock(idx, CounterCheck);
        }
    }

    // Complete chunk counter PHI
    ChunkCounter->addIncoming(ConstantInt::get(I64Ty, 0), OuterHeader);
    ChunkCounter->addIncoming(CounterNext, CounterCheck);

    // ── Phase 10: Build outer.latch — forward loop-carried values ──
    IRBuilder<> OLB(OuterLatch);

    // Capture loop-carried values (originally from Latch, now from CounterCheck)
    SmallVector<Value *, 8> forwardedValues;
    for (auto &info : headerPhiForwarding) {
        int idx = info.headerPhi->getBasicBlockIndex(CounterCheck);
        Value *CarriedVal = info.headerPhi->getIncomingValue(idx);
        forwardedValues.push_back(CarriedVal);
    }
    OLB.CreateBr(OuterHeader);

    // ── Phase 11: Complete outer.header PHIs (init from Preheader, fwd from OuterLatch) ──
    for (unsigned i = 0; i < headerPhiForwarding.size(); i++) {
        auto &info = headerPhiForwarding[i];
        info.outerPhi->addIncoming(info.initVal, Preheader);
        info.outerPhi->addIncoming(forwardedValues[i], OuterLatch);
    }

    // ── Phase 12: Update LoopInfo ──
    Loop *ParentLoop = L->getParentLoop();
    Loop *OuterLoop = LI.AllocateLoop();

    if (ParentLoop) {
        for (auto I = ParentLoop->begin(); I != ParentLoop->end(); ++I) {
            if (*I == L) { ParentLoop->removeChildLoop(I); break; }
        }
        ParentLoop->addChildLoop(OuterLoop);
    } else {
        for (auto I = LI.begin(); I != LI.end(); ++I) {
            if (*I == L) { LI.removeLoop(I); break; }
        }
        LI.addTopLevelLoop(OuterLoop);
    }
    OuterLoop->addChildLoop(L);

    OuterLoop->addBasicBlockToLoop(OuterHeader, LI);
    OuterLoop->addBasicBlockToLoop(OuterLatch, LI);
    L->addBasicBlockToLoop(CounterCheck, LI);

    for (BasicBlock *BB : L->blocks())
        OuterLoop->addBlockEntry(BB);

    OuterLoop->moveToHeader(OuterHeader);

    // ── Phase 13: Rebuild DomTree ──
    DT.recalculate(*F);

    // ── Phase 14: Set metadata on generated loops ──
    removeLoopTripCountMetadata(L);
    setLoopTripCountMetadata(L, K);
    setStripMinedLoopMetadata(L);
    if (NUpper > 0) {
        // Chunk outer loop executes at most ceil(NUpper / K) times.
        uint64_t outerTripCountUpper = 1 + ((NUpper - 1) / K);
        setLoopTripCountMetadata(OuterLoop, outerTripCountUpper);
    } else {
        removeLoopTripCountMetadata(OuterLoop);
    }

    // ── Phase 15: LCSSA repair, SCEV invalidation ──
    formLCSSARecursively(*OuterLoop, DT, &LI, &SE);
    SE.forgetLoop(L);

    return true;
}

static void printSummary(const Function &F, const LoopStripMiningStats &stats) {
    errs() << "=== Loop Strip-Mining: " << F.getName() << " ===\n";
    errs() << "  Loops considered:                " << stats.loopsSeen << "\n";
    errs() << "  Eligible loops:                  " << stats.loopsEligible << "\n";
    errs() << "  Rewritten loops:                 " << stats.loopsRewritten << "\n";
    errs() << "    Strip-mined:                   "
           << (stats.loopsRewritten - stats.loopsChunked) << "\n";
    errs() << "    Chunked:                       " << stats.loopsChunked << "\n";

    unsigned skippedTotal = 0;
    for (const auto &entry : stats.skippedReasons) {
        skippedTotal += entry.second;
    }
    errs() << "  Skipped loops:                   " << skippedTotal << "\n";

    if (!stats.skippedReasons.empty()) {
        errs() << "  Skipped reason histogram:\n";
        for (const auto &entry : stats.skippedReasons) {
            errs() << "    - " << entry.first << ": " << entry.second << "\n";
        }
    }

    if (!stats.chosenKByHeader.empty()) {
        errs() << "  Chosen K values:\n";
        for (const auto &[header, k] : stats.chosenKByHeader) {
            errs() << "    - " << header << ": K=" << k << "\n";
        }
    }
}

} // anonymous namespace

namespace checkpoint {

PreservedAnalyses LoopStripMiningPass::run(Function &F,
                                        FunctionAnalysisManager &AM) {
    if (F.isDeclaration()) {
        return PreservedAnalyses::all();
    }

    if (MILPConfigOpt.getValue().empty()) {
        if (LoopStripMiningEnabledOpt) {
            errs() << "LoopStripMiningPass: missing -milp-config; skipping "
                   << F.getName() << "\n";
        }
        return PreservedAnalyses::all();
    }

    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt) {
        errs() << "LoopStripMiningPass: failed to parse MILP config for "
               << F.getName() << "; skipping\n";
        return PreservedAnalyses::all();
    }
    bool loopStripMiningEnabled =
        LoopStripMiningEnabledOpt.getValue() || milpParamsOpt->loopStripMiningEnabled;
    if (!loopStripMiningEnabled) {
        return PreservedAnalyses::all();
    }

    if (EnergyConfigOpt.getValue().empty()) {
        errs() << "LoopStripMiningPass: missing -energy-config; skipping "
               << F.getName() << "\n";
        return PreservedAnalyses::all();
    }

    auto factory = EnergyEstimatorFactory::createDefault();
    std::unique_ptr<EnergyEstimator> estimator =
        factory.createFromConfig(EnergyConfigOpt.getValue());
    if (!estimator) {
        errs() << "LoopStripMiningPass: failed to create estimator for "
               << F.getName() << "; skipping\n";
        return PreservedAnalyses::all();
    }
    estimator->prepareForFunction(F);

    DenseMap<const BasicBlock *, double> blockEnergy;
    blockEnergy.reserve(F.size());
    for (BasicBlock &BB : F) {
        blockEnergy[&BB] = estimator->estimate(BB).cost;
    }

    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
    auto &AC = AM.getResult<AssumptionAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);
    auto &TTI = AM.getResult<TargetIRAnalysis>(F);
    checkpoint::CFGAnalysis cfg(F, LI, *estimator);
    checkpoint::StateAnalysis state(F, AA, cfg);
    if (state.hasAnalysisErrors()) {
        state.printAnalysisErrors(errs());
        errs() << "LoopStripMiningPass: skipping " << F.getName()
               << " due to unresolved memory/call effects.\n";
        return PreservedAnalyses::all();
    }

    LoopStripMiningStats stats;
    bool changed = false;

    // Select loops to strip-mine (outermost-first within each nest).
    // Snapshot headers as WeakTrackingVH so we can detect invalidation
    // from prior rewrites that may restructure the CFG.
    auto selected = selectLoopsToStripMine(
        LI, SE, blockEnergy, *milpParamsOpt, state, stats);
    std::vector<std::pair<WeakTrackingVH, LoopRewritePlan>> worklist;
    worklist.reserve(selected.size());
    for (auto &[L, plan] : selected) {
        worklist.emplace_back(WeakTrackingVH(L->getHeader()), plan);
    }

    for (auto &[headerHandle, plan] : worklist) {
        BasicBlock *Header = dyn_cast_or_null<BasicBlock>(headerHandle);
        if (!Header) {
            stats.skippedReasons["loop-header-erased-after-prior-rewrite"]++;
            continue;
        }

        std::string headerName = checkpoint::getBlockName(*Header, F);
        Loop *L = LI.getLoopFor(Header);
        if (!L || L->getHeader() != Header) {
            stats.skippedReasons["loop-unresolvable-from-header"]++;
            continue;
        }

        plan.L = L;
        stats.loopsEligible++;

        bool rewritten = plan.isChunking
            ? chunkLoop(plan, LI, SE, DT)
            : stripMineLoop(plan, LI, SE, DT, AC, AA, TTI);
        if (!rewritten) {
            stats.skippedReasons["rewrite-utility-failed"]++;
            if (LoopStripMiningVerboseOpt) {
                errs() << "LoopStripMiningPass: rewrite failed " << F.getName()
                       << "::" << headerName << " K=" << plan.K << "\n";
            }
            continue;
        }

        if (plan.isChunking) {
            // Re-estimate on the transformed loop to include chunking overhead
            // (counter.check block) and clamp K in place if needed.
            refreshBlockEnergy(F, *estimator, blockEnergy);
            checkpoint::CFGAnalysis postCfg(F, LI, *estimator);
            checkpoint::StateAnalysis postState(F, AA, postCfg);
            const checkpoint::StateAnalysis *reclampState = &state;
            if (postState.hasAnalysisErrors()) {
                if (LoopStripMiningVerboseOpt) {
                    errs() << "LoopStripMiningPass: post-chunk state analysis "
                           << "failed for re-clamp in " << F.getName()
                           << "; using pre-rewrite state margins\n";
                }
            } else {
                reclampState = &postState;
            }
            ChunkBudgetResult reclamp = recomputeChunkKWithOverhead(
                L, blockEnergy, *milpParamsOpt, LI, SE, *reclampState);
            if (!reclamp.ok) {
                stats.skippedReasons["chunk-k-reclamp-unavailable"]++;
                if (LoopStripMiningVerboseOpt) {
                    errs() << "LoopStripMiningPass: chunk K re-clamp unavailable "
                           << F.getName() << "::" << headerName
                           << " reason=" << reclamp.error
                           << " original-K=" << plan.K << "\n";
                }
            } else {
                plan.iterEnergy = reclamp.iterEnergy;
                uint64_t newK = std::min<uint64_t>(plan.K, reclamp.maxK);
                if (newK != plan.K) {
                    if (!updateChunkLoopBound(L, newK)) {
                        stats.skippedReasons["chunk-k-reclamp-update-failed"]++;
                        if (LoopStripMiningVerboseOpt) {
                            errs() << "LoopStripMiningPass: chunk K re-clamp update failed "
                                   << F.getName() << "::" << headerName
                                   << " original-K=" << plan.K
                                   << " new-K=" << newK << "\n";
                        }
                    } else {
                        setLoopTripCountMetadata(L, newK);
                        SE.forgetLoop(L);
                        if (LoopStripMiningVerboseOpt) {
                            errs() << "LoopStripMiningPass: chunk K re-clamped "
                                   << F.getName() << "::" << headerName
                                   << " original-K=" << plan.K
                                   << " new-K=" << newK
                                   << " E_iter_wc_post=" << reclamp.iterEnergy
                                   << "\n";
                        }
                        plan.K = newK;
                    }
                }
            }
        }

        changed = true;
        stats.loopsRewritten++;
        if (plan.isChunking)
            stats.loopsChunked++;
        stats.chosenKByHeader.emplace_back(headerName, plan.K);
        if (LoopStripMiningVerboseOpt) {
            errs() << "LoopStripMiningPass: "
                   << (plan.isChunking ? "chunked " : "rewritten ")
                   << F.getName() << "::" << headerName
                   << " N=" << plan.N << " K=" << plan.K
                   << " E_iter_wc=" << plan.iterEnergy << "\n";
        }
    }

    if (verifyFunction(F, &errs())) {
        errs() << "LoopStripMiningPass: verifier reported errors in "
               << F.getName() << "\n";
    }

    printSummary(F, stats);
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace checkpoint
