#include "milp/LoopStripMiningPass.h"

#include "common/AnnotationUtils.h"
#include "common/BlockUtils.h"
#include "common/LoopTripCount.h"
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
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
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

enum class VisitState {
    Unvisited = 0,
    Visiting,
    Visited,
};

struct LoopStripMiningStats {
    unsigned loopsSeen = 0;
    unsigned loopsEligible = 0;
    unsigned loopsRewritten = 0;
    std::map<std::string, unsigned> skippedReasons;
    std::vector<std::pair<std::string, uint64_t>> chosenKByHeader;
};


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

static Loop *getDirectChildLoop(const Loop *Parent, const BasicBlock *BB,
                                const LoopInfo &LI) {
    Loop *Inner = LI.getLoopFor(BB);
    if (!Inner || Inner == Parent) return nullptr;
    while (Inner->getParentLoop() != Parent) Inner = Inner->getParentLoop();
    return Inner;
}

using checkpoint::getMarkerTripCount;
using checkpoint::removeLoopTripCountMetadata;
using checkpoint::removeStripMinedLoopMetadata;
using checkpoint::setLoopTripCountMetadata;
using checkpoint::setStripMinedLoopMetadata;

static std::optional<uint64_t> getConstantTripCount(Loop *L,
                                                    ScalarEvolution &SE,
                                                    const BasicBlock *ExitingBlock);

static EnergyPathResult computeWorstCaseIterationEnergy(
    Loop *L,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    LoopInfo &LI,
    ScalarEvolution &SE) {
    EnergyPathResult result;

    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    if (!Header || !Latch) {
        result.error = "missing-header-or-latch";
        return result;
    }

    // Step 1: Recursively compute total energy for each direct sub-loop
    DenseMap<const Loop *, double> subLoopTotal;
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
    }

    // Step 2: Block energy with sub-loop collapsing
    auto getEnergy = [&](const BasicBlock *BB) -> double {
        Loop *ChildL = getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            auto it = subLoopTotal.find(ChildL);
            return (it != subLoopTotal.end()) ? it->second : 0.0;
        }
        auto it = blockEnergy.find(BB);
        return (it != blockEnergy.end()) ? it->second : 0.0;
    };

    // Step 3: Successor computation with sub-loop collapsing
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
        return result;
    }

    DenseMap<const BasicBlock *, VisitState> visitState;
    DenseMap<const BasicBlock *, double> memo;
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
        double bestSucc = -1.0;
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
            if (succEnergy > bestSucc) {
                bestSucc = succEnergy;
            }
        }
        visitState[BB] = VisitState::Visited;

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

/// Compute a conservative upper bound on the ineligible restore cost for
/// objects relevant to loop \p L.  This mirrors the classification in
/// StateAnalysis but is scoped to the loop so the estimate is tighter than
/// a whole-function sum.  The result is an upper bound because it counts
/// every potentially-ineligible object touching the loop regardless of
/// simultaneous liveness.
static double estimateIneligRestoreUpperBound(
    Loop *L, const checkpoint::MILPEnergyParams &params) {
    Function *F = L->getHeader()->getParent();
    Module *M = F->getParent();
    const DataLayout &DL = M->getDataLayout();
    double total = 0.0;

    // --- Non-candidate globals accessed in the loop ---
    std::set<GlobalVariable *> seenGV;
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            const Value *Ptr = nullptr;
            if (auto *LI = dyn_cast<LoadInst>(&I))
                Ptr = LI->getPointerOperand();
            else if (auto *SI = dyn_cast<StoreInst>(&I))
                Ptr = SI->getPointerOperand();
            else
                continue;

            const Value *Obj =
                getUnderlyingObject(Ptr->stripPointerCasts());
            auto *GV = dyn_cast<GlobalVariable>(const_cast<Value *>(Obj));
            if (!GV)
                continue;

            if (GV->isDeclaration())
                continue;
            if (GV->isConstant())
                continue;
            if (GV->getName().starts_with("llvm."))
                continue;
            if (GV->getName().starts_with("__nvm_"))
                continue;
            if (GV->getName().starts_with("__vm_shadow_"))
                continue;
            if (!GV->getValueType()->isSized())
                continue;
            if (checkpoint::isMilpCandidateAnnotated(GV, M))
                continue;

            if (!seenGV.insert(GV).second)
                continue;

            total += static_cast<double>(DL.getTypeAllocSize(GV->getValueType()))
                     * params.memRestoreEnergyPerByte;
        }
    }

    // --- Stack allocas used in the loop ---
    std::set<AllocaInst *> seenAlloca;
    BasicBlock &EntryBB = F->getEntryBlock();
    for (Instruction &I : EntryBB) {
        auto *AI = dyn_cast<AllocaInst>(&I);
        if (!AI)
            continue;
        if (!AI->getAllocatedType()->isSized())
            continue;

        bool usedInLoop = false;
        for (const User *U : AI->users()) {
            if (auto *UI = dyn_cast<Instruction>(U)) {
                if (L->contains(UI->getParent())) {
                    usedInLoop = true;
                    break;
                }
            }
        }
        if (!usedInLoop)
            continue;
        if (!seenAlloca.insert(AI).second)
            continue;

        total += static_cast<double>(DL.getTypeAllocSize(AI->getAllocatedType()))
                 * params.memRestoreEnergyPerByte;
    }

    // --- SSA values defined inside the loop with cross-block uses ---
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            if (I.getType()->isVoidTy())
                continue;
            if (isa<AllocaInst>(&I))
                continue;
            if (!I.getType()->isSized())
                continue;

            bool hasCrossBlockUse = false;
            for (const User *U : I.users()) {
                if (auto *UI = dyn_cast<Instruction>(U)) {
                    if (UI->getParent() != BB) {
                        hasCrossBlockUse = true;
                        break;
                    }
                }
            }
            if (!hasCrossBlockUse)
                continue;

            total += static_cast<double>(DL.getTypeAllocSize(I.getType()))
                     * params.memRestoreEnergyPerByte;
        }
    }

    // --- SSA values defined outside the loop but used inside ---
    std::set<Value *> seenLiveIn;
    for (BasicBlock *BB : L->blocks()) {
        for (Instruction &I : *BB) {
            for (Use &Op : I.operands()) {
                auto *DefI = dyn_cast<Instruction>(Op.get());
                if (!DefI)
                    continue;
                if (L->contains(DefI->getParent()))
                    continue;
                if (isa<AllocaInst>(DefI))
                    continue;
                if (DefI->getType()->isVoidTy())
                    continue;
                if (!DefI->getType()->isSized())
                    continue;

                bool hasCrossBlockUse = false;
                for (const User *U : DefI->users()) {
                    if (auto *UI = dyn_cast<Instruction>(U)) {
                        if (UI->getParent() != DefI->getParent()) {
                            hasCrossBlockUse = true;
                            break;
                        }
                    }
                }
                if (!hasCrossBlockUse)
                    continue;

                if (!seenLiveIn.insert(static_cast<Value *>(DefI)).second)
                    continue;

                total += static_cast<double>(DL.getTypeAllocSize(DefI->getType()))
                         * params.memRestoreEnergyPerByte;
            }
        }
    }

    return total;
}

static PlanResult buildRewritePlan(
    Loop *L,
    ScalarEvolution &SE,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params,
    LoopInfo &LI) {
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

    // Tighten budget by:
    // (1) upper-bound ineligible restore cost, and
    // (2) user-configured capacitor margin for strip-mining.
    // This keeps chosen K conservative so summarized loop chunks are more
    // likely to remain MILP-feasible.
    double ineligMargin = estimateIneligRestoreUpperBound(L, params);
    double stripMiningMargin =
        params.capacity * (params.loopStripMiningMarginPercent / 100.0);
    double effectiveBudget = budget - ineligMargin - stripMiningMargin;
    if (effectiveBudget <= 0.0) {
        result.skipReason = "nonpositive-effective-budget";
        return result;
    }

    EnergyPathResult iterEnergy =
        computeWorstCaseIterationEnergy(L, blockEnergy, LI, SE);
    if (!iterEnergy.ok) {
        result.skipReason = iterEnergy.error;
        return result;
    }
    if (iterEnergy.energy <= 0.0) {
        result.skipReason = "nonpositive-iteration-energy";
        return result;
    }

    double rawK = std::floor(effectiveBudget / iterEnergy.energy);
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

static void selectInNest(
    Loop *L, ScalarEvolution &SE,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params, LoopInfo &LI,
    std::vector<std::pair<Loop *, LoopRewritePlan>> &out,
    LoopStripMiningStats &stats) {

    stats.loopsSeen++;
    PlanResult pr = buildRewritePlan(L, SE, blockEnergy, params, LI);

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
               << headerName << " reason=" << pr.skipReason << "\n";
    }

    if (pr.skipReason == "k-covers-entire-loop") {
        stats.skippedReasons[pr.skipReason]++;
        return;
    }

    stats.skippedReasons[pr.skipReason]++;
    for (Loop *SubL : L->getSubLoops()) {
        selectInNest(SubL, SE, blockEnergy, params, LI, out, stats);
    }
}

static std::vector<std::pair<Loop *, LoopRewritePlan>> selectLoopsToStripMine(
    LoopInfo &LI, ScalarEvolution &SE,
    const DenseMap<const BasicBlock *, double> &blockEnergy,
    const checkpoint::MILPEnergyParams &params,
    LoopStripMiningStats &stats) {
    std::vector<std::pair<Loop *, LoopRewritePlan>> selected;
    for (Loop *L : LI) {
        selectInNest(L, SE, blockEnergy, params, LI, selected, stats);
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
    struct HeaderPhiInfo {
        PHINode *headerPhi;
        PHINode *outerPhi;
        Value   *initVal;
    };
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

static void printSummary(const Function &F, const LoopStripMiningStats &stats) {
    errs() << "=== Loop Strip-Mining: " << F.getName() << " ===\n";
    errs() << "  Loops considered:                " << stats.loopsSeen << "\n";
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

    LoopStripMiningStats stats;
    bool changed = false;

    // Select loops to strip-mine (outermost-first within each nest).
    // Snapshot headers as WeakTrackingVH so we can detect invalidation
    // from prior rewrites that may restructure the CFG.
    auto selected = selectLoopsToStripMine(LI, SE, blockEnergy, *milpParamsOpt, stats);
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

        bool rewritten =
            stripMineLoop(plan, LI, SE, DT, AC, AA, TTI);
        if (!rewritten) {
            stats.skippedReasons["rewrite-utility-failed"]++;
            if (LoopStripMiningVerboseOpt) {
                errs() << "LoopStripMiningPass: rewrite failed " << F.getName()
                       << "::" << headerName << " K=" << plan.K << "\n";
            }
            continue;
        }

        changed = true;
        stats.loopsRewritten++;
        stats.chosenKByHeader.emplace_back(headerName, plan.K);
        if (LoopStripMiningVerboseOpt) {
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
