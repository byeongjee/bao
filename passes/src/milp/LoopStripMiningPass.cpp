#include "milp/LoopStripMiningPass.h"

#include "common/BlockUtils.h"
#include "estimator/EnergyEstimatorFactory.h"
#include "milp/EnergyModel.h"

#include "llvm/ADT/DenseMap.h"
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
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> MILPConfigOpt;
extern cl::opt<bool> LoopChunkingEnabledOpt;

namespace {

static cl::opt<bool> LoopChunkingVerboseOpt(
    "loop-chunking-verbose",
    cl::desc("Print per-loop strip-mining decisions"),
    cl::init(false));

struct LoopRewritePlan {
    Loop *L = nullptr;
    uint64_t N = 0;
    uint64_t K = 0;
    double iterEnergy = 0.0;
};

struct PlanResult {
    std::optional<LoopRewritePlan> plan;
    std::string skipReason;
};

struct EnergyPathResult {
    bool ok = false;
    double energy = 0.0;
    std::string error;
};

struct StripMiningStats {
    unsigned loopsSeen = 0;
    unsigned loopsEligible = 0;
    unsigned loopsRewritten = 0;
    std::map<std::string, unsigned> skippedReasons;
    std::vector<std::pair<std::string, uint64_t>> chosenKByHeader;
};

static void collectInnermostLoopHeaders(const Loop *L,
                                        const Function &F,
                                        std::vector<std::string> &out) {
    for (const Loop *Sub : L->getSubLoops()) {
        collectInnermostLoopHeaders(Sub, F, out);
    }
    if (L->getSubLoops().empty()) {
        out.push_back(checkpoint::getBlockName(*L->getHeader(), F));
    }
}

static std::vector<std::string> collectInnermostLoopHeaders(const LoopInfo &LI,
                                                            const Function &F) {
    std::vector<std::string> headers;
    for (const Loop *L : LI) {
        collectInnermostLoopHeaders(L, F, headers);
    }
    return headers;
}

static Loop *findInnermostLoopByHeaderName(Loop *L,
                                           const Function &F,
                                           StringRef headerName) {
    for (Loop *Sub : L->getSubLoops()) {
        if (Loop *Found = findInnermostLoopByHeaderName(Sub, F, headerName)) {
            return Found;
        }
    }
    if (!L->getSubLoops().empty()) {
        return nullptr;
    }
    if (checkpoint::getBlockName(*L->getHeader(), F) == headerName) {
        return L;
    }
    return nullptr;
}

static Loop *findInnermostLoopByHeaderName(LoopInfo &LI,
                                           const Function &F,
                                           StringRef headerName) {
    for (Loop *L : LI) {
        if (Loop *Found = findInnermostLoopByHeaderName(L, F, headerName)) {
            return Found;
        }
    }
    return nullptr;
}

static bool containsInvoke(const Loop *L) {
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            if (isa<InvokeInst>(I)) {
                return true;
            }
        }
    }
    return false;
}

static bool isLoopTripcountCall(const Instruction &I) {
    auto *CI = dyn_cast<CallInst>(&I);
    if (!CI) {
        return false;
    }
    const Function *Callee = CI->getCalledFunction();
    return Callee && Callee->getName() == "__loop_tripcount";
}

static void removeLoopTripcountMarkers(Loop *L) {
    SmallVector<Instruction *, 8> toErase;
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            if (isLoopTripcountCall(I)) {
                toErase.push_back(&I);
            }
        }
    }
    for (Instruction *I : toErase) {
        I->eraseFromParent();
    }
}

static void insertLoopTripcountMarker(Loop *L, uint64_t tripCount) {
    if (tripCount == 0) {
        return;
    }

    BasicBlock *Header = L->getHeader();
    auto insertPt = Header->getFirstInsertionPt();
    if (insertPt == Header->end()) {
        return;
    }

    Module *M = Header->getModule();
    LLVMContext &Ctx = M->getContext();
    auto *I32Ty = Type::getInt32Ty(Ctx);
    auto *MarkerTy = FunctionType::get(Type::getVoidTy(Ctx), {I32Ty}, false);
    FunctionCallee Marker = M->getOrInsertFunction("__loop_tripcount", MarkerTy);

    uint64_t maxMarker = static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
    uint64_t markerVal = std::min(tripCount, maxMarker);

    IRBuilder<> B(&*insertPt);
    B.CreateCall(Marker, {ConstantInt::get(I32Ty, markerVal)});
}

static EnergyPathResult computeWorstCaseIterationEnergy(
    Loop *L,
    const DenseMap<const BasicBlock *, double> &blockEnergy) {
    EnergyPathResult result;

    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    if (!Header || !Latch) {
        result.error = "missing-header-or-latch";
        return result;
    }

    auto getEnergy = [&](const BasicBlock *BB) -> double {
        auto it = blockEnergy.find(BB);
        if (it == blockEnergy.end()) {
            return 0.0;
        }
        return it->second;
    };

    if (Header == Latch) {
        result.ok = true;
        result.energy = getEnergy(Header);
        return result;
    }

    DenseMap<const BasicBlock *, unsigned> visitState;
    DenseMap<const BasicBlock *, double> memo;
    bool cycleDetected = false;

    std::function<double(const BasicBlock *)> dfs =
        [&](const BasicBlock *BB) -> double {
        if (BB == Latch) {
            return getEnergy(BB);
        }

        unsigned &state = visitState[BB];
        if (state == 1) {
            cycleDetected = true;
            return -1.0;
        }
        if (state == 2) {
            return memo[BB];
        }

        state = 1;
        double bestSucc = -1.0;
        for (const BasicBlock *Succ : successors(BB)) {
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
            if (succEnergy > bestSucc) {
                bestSucc = succEnergy;
            }
        }
        state = 2;

        if (bestSucc < 0.0) {
            memo[BB] = -1.0;
            return -1.0;
        }

        memo[BB] = getEnergy(BB) + bestSucc;
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

    result.ok = true;
    result.energy = energy;
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

static PlanResult buildRewritePlan(
    Loop *L,
    ScalarEvolution &SE,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params) {
    PlanResult result;
    result.skipReason = "unknown";

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
    if (!L->getCanonicalInductionVariable()) {
        result.skipReason = "missing-canonical-induction";
        return result;
    }

    SmallVector<BasicBlock *, 8> exitingBlocks;
    L->getExitingBlocks(exitingBlocks);
    if (exitingBlocks.size() != 1) {
        result.skipReason = "requires-single-exiting-block";
        return result;
    }

    if (containsInvoke(L)) {
        result.skipReason = "contains-invoke";
        return result;
    }

    std::optional<uint64_t> tripCount =
        getConstantTripCount(L, SE, exitingBlocks.front());
    if (!tripCount) {
        result.skipReason = "nonconstant-trip-count";
        return result;
    }
    if (*tripCount < 2) {
        result.skipReason = "trip-count-too-small";
        return result;
    }

    double budget = params.capacity - params.E_pro - params.E_epi;
    if (budget <= 0.0) {
        result.skipReason = "nonpositive-energy-budget";
        return result;
    }

    EnergyPathResult iterEnergy =
        computeWorstCaseIterationEnergy(L, blockEnergy);
    if (!iterEnergy.ok) {
        result.skipReason = iterEnergy.error;
        return result;
    }
    if (iterEnergy.energy <= 0.0) {
        result.skipReason = "nonpositive-iteration-energy";
        return result;
    }

    double rawK = std::floor(budget / iterEnergy.energy);
    if (!std::isfinite(rawK) || rawK <= 0.0) {
        result.skipReason = "k-zero";
        return result;
    }

    uint64_t K = static_cast<uint64_t>(rawK);
    if (K <= 1) {
        result.skipReason = "k-not-beneficial";
        return result;
    }
    if (K >= *tripCount) {
        result.skipReason = "k-covers-entire-loop";
        return result;
    }
    if (K > std::numeric_limits<unsigned>::max()) {
        result.skipReason = "k-too-large";
        return result;
    }

    LoopRewritePlan plan;
    plan.L = L;
    plan.N = *tripCount;
    plan.K = K;
    plan.iterEnergy = iterEnergy.energy;
    result.plan = plan;
    result.skipReason.clear();
    return result;
}

static bool rewriteLoopWithChunkSize(const LoopRewritePlan &plan,
                                     LoopInfo &LI,
                                     ScalarEvolution &SE,
                                     DominatorTree &DT,
                                     AssumptionCache &AC,
                                     AAResults &AA,
                                     const TargetTransformInfo &TTI) {
    UnrollLoopOptions opts;
    opts.Count = static_cast<unsigned>(plan.K);
    opts.Force = true;
    opts.Runtime = false;
    opts.AllowExpensiveTripCount = true;
    opts.UnrollRemainder = true;
    opts.ForgetAllSCEV = false;
    opts.Heart = nullptr;
    opts.SCEVExpansionBudget = 32;
    opts.RuntimeUnrollMultiExit = false;
    opts.AddAdditionalAccumulators = false;

    Loop *remainderLoop = nullptr;
    LoopUnrollResult unrollResult =
        UnrollLoop(plan.L, opts, &LI, &SE, &DT, &AC, &TTI, nullptr,
                   /*PreserveLCSSA=*/true, &remainderLoop, &AA);
    if (unrollResult == LoopUnrollResult::Unmodified) {
        return false;
    }

    if (unrollResult == LoopUnrollResult::PartiallyUnrolled) {
        uint64_t mainTrip = plan.N / plan.K;
        uint64_t remTrip = plan.N % plan.K;

        removeLoopTripcountMarkers(plan.L);
        insertLoopTripcountMarker(plan.L, mainTrip);

        if (remainderLoop) {
            removeLoopTripcountMarkers(remainderLoop);
            insertLoopTripcountMarker(remainderLoop, remTrip);
        }
    }

    return true;
}

static void printSummary(const Function &F, const StripMiningStats &stats) {
    errs() << "=== Loop Strip-Mining: " << F.getName() << " ===\n";
    errs() << "  Innermost loops seen:            " << stats.loopsSeen << "\n";
    errs() << "  Eligible loops:                  " << stats.loopsEligible << "\n";
    errs() << "  Rewritten loops:                 " << stats.loopsRewritten << "\n";

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
    if (!LoopChunkingEnabledOpt) {
        return PreservedAnalyses::all();
    }

    if (F.isDeclaration()) {
        return PreservedAnalyses::all();
    }

    if (EnergyConfigOpt.getValue().empty()) {
        errs() << "LoopStripMiningPass: missing -energy-config; skipping "
               << F.getName() << "\n";
        return PreservedAnalyses::all();
    }
    if (MILPConfigOpt.getValue().empty()) {
        errs() << "LoopStripMiningPass: missing -milp-config; skipping "
               << F.getName() << "\n";
        return PreservedAnalyses::all();
    }

    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt) {
        errs() << "LoopStripMiningPass: failed to parse MILP config for "
               << F.getName() << "; skipping\n";
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

    StripMiningStats stats;
    bool changed = false;

    std::vector<std::string> worklist = collectInnermostLoopHeaders(LI, F);
    stats.loopsSeen = static_cast<unsigned>(worklist.size());

    for (const std::string &headerName : worklist) {
        Loop *L = findInnermostLoopByHeaderName(LI, F, headerName);
        if (!L) {
            stats.skippedReasons["loop-not-found-after-prior-rewrite"]++;
            continue;
        }

        PlanResult planResult =
            buildRewritePlan(L, SE, blockEnergy, *milpParamsOpt);
        if (!planResult.plan) {
            stats.skippedReasons[planResult.skipReason]++;
            if (LoopChunkingVerboseOpt) {
                errs() << "LoopStripMiningPass: skip " << F.getName() << "::"
                       << headerName << " reason=" << planResult.skipReason << "\n";
            }
            continue;
        }

        stats.loopsEligible++;
        const LoopRewritePlan &plan = *planResult.plan;

        bool rewritten =
            rewriteLoopWithChunkSize(plan, LI, SE, DT, AC, AA, TTI);
        if (!rewritten) {
            stats.skippedReasons["rewrite-utility-failed"]++;
            if (LoopChunkingVerboseOpt) {
                errs() << "LoopStripMiningPass: rewrite failed " << F.getName()
                       << "::" << headerName << " K=" << plan.K << "\n";
            }
            continue;
        }

        changed = true;
        stats.loopsRewritten++;
        stats.chosenKByHeader.emplace_back(headerName, plan.K);
        if (LoopChunkingVerboseOpt) {
            errs() << "LoopStripMiningPass: rewritten " << F.getName() << "::"
                   << headerName << " N=" << plan.N << " K=" << plan.K
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
