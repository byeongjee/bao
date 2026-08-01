#include "milp/LoopStripMiningPass.h"

#include "common/BlockUtils.h"
#include "common/Logger.h"
#include "common/LoopTripCount.h"
#include "common/LoopUtils.h"
#include "common/PassStatistics.h"
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
#include "llvm/Support/JSON.h"
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

cl::opt<std::string>
    LoopStripMiningStatsJsonOpt("loop-strip-mining-stats-json",
                                cl::desc("Path to write loop strip-mining diagnostics JSON"),
                                cl::value_desc("filename"), cl::init(""));

struct LoopRewritePlan {
    Loop *L = nullptr;
    uint64_t N = 0;
    uint64_t K = 0;
    double iterEnergy = 0.0;
    bool isChunking = false;
};

struct LoopStripMiningDetail {
    std::string functionName;
    std::string loopHeader;
    std::string decision;
    std::string skipReason;
    std::string skipDetail;
    bool isChunking = false;
    bool tripCountKnown = false;
    uint64_t tripCount = 0;
    double budget = 0.0;
    double iterEnergy = 0.0;
    double perIterNvmPenalty = 0.0;
    double loopStripMiningCost = 0.0;
    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = 0.0;
    double budgetAfterBoundary = 0.0;
    double perIterTotalEnergy = 0.0;
    double strictBudget = 0.0;
    double rawK = 0.0;
    bool candidateKValid = false;
    uint64_t candidateK = 0;
    bool chosenKValid = false;
    uint64_t chosenK = 0;
    bool rewriteAttempted = false;
    bool rewriteSucceeded = false;
    std::string rewriteFailureReason;
    bool postChunkReclampAttempted = false;
    bool postChunkReclampSucceeded = false;
    bool postChunkReclampApplied = false;
    bool postChunkMaxKValid = false;
    uint64_t postChunkMaxK = 0;
    bool postChunkIterEnergyValid = false;
    double postChunkIterEnergy = 0.0;
    std::string postChunkReclampError;
};

struct PlanResult {
    std::optional<LoopRewritePlan> plan;
    std::string skipReason;
    std::string skipDetail;
    LoopStripMiningDetail detail;
};

struct SelectedLoopPlan {
    Loop *L = nullptr;
    LoopRewritePlan plan;
    size_t detailIndex = 0;
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
    std::vector<LoopStripMiningDetail> loopDetails;
};

/// Per-function copy of the stats, retained across functions for the
/// module-level JSON dump.
using LoopStripMiningFunctionSnapshot = LoopStripMiningStats;

struct HeaderPhiInfo {
    PHINode *headerPhi;
    PHINode *outerPhi;
    Value *initVal;
};

using checkpoint::containsInvoke;
using checkpoint::getDirectChildLoop;
using checkpoint::getMarkerTripCount;
using checkpoint::hasStripMinedLoopMetadata;
using checkpoint::removeLoopTripCountMetadata;
using checkpoint::setLoopTripCountMetadata;
using checkpoint::setStripMinedLoopMetadata;

static std::string getLoopHeaderName(const Loop *L) {
    if (!L)
        return "<unknown>";
    const BasicBlock *Header = L->getHeader();
    const Function *F = Header ? Header->getParent() : nullptr;
    if (!Header || !F)
        return "<unknown>";
    return checkpoint::getBlockName(*Header, *F);
}

static std::string getLoopFunctionName(const Loop *L) {
    if (!L)
        return "<unknown>";
    const BasicBlock *Header = L->getHeader();
    const Function *F = Header ? Header->getParent() : nullptr;
    return F ? F->getName().str() : "<unknown>";
}

static bool isChunkCounterLoop(const Loop *L) {
    if (!L || !L->getHeader())
        return false;

    for (const PHINode &PN : L->getHeader()->phis()) {
        if (PN.getName().starts_with("chunk.counter"))
            return true;
    }
    return false;
}

static LoopStripMiningDetail buildInitialLoopDetail(const Loop *L) {
    LoopStripMiningDetail detail;
    detail.functionName = getLoopFunctionName(L);
    detail.loopHeader = getLoopHeaderName(L);
    detail.decision = "skipped";
    return detail;
}

static json::Object loopDetailToJSON(const LoopStripMiningDetail &detail) {
    json::Object obj;
    obj["function"] = detail.functionName;
    obj["loop_header"] = detail.loopHeader;
    obj["decision"] = detail.decision;
    obj["skip_reason"] = detail.skipReason;
    obj["skip_detail"] = detail.skipDetail;
    obj["is_chunking"] = detail.isChunking;
    obj["trip_count_known"] = detail.tripCountKnown;
    if (detail.tripCountKnown)
        obj["trip_count"] = static_cast<int64_t>(detail.tripCount);
    obj["budget"] = detail.budget;
    obj["iter_energy"] = detail.iterEnergy;
    obj["per_iter_nvm_penalty"] = detail.perIterNvmPenalty;
    obj["loop_strip_mining_cost"] = detail.loopStripMiningCost;
    obj["restore_livein_margin"] = detail.restoreLiveInMargin;
    obj["commit_def_margin"] = detail.commitDefMargin;
    obj["boundary_state_margin"] = detail.boundaryStateMargin;
    obj["budget_after_boundary"] = detail.budgetAfterBoundary;
    obj["per_iter_total_energy"] = detail.perIterTotalEnergy;
    obj["strict_budget"] = detail.strictBudget;
    obj["raw_k"] = detail.rawK;
    obj["candidate_k_valid"] = detail.candidateKValid;
    if (detail.candidateKValid)
        obj["candidate_k"] = static_cast<int64_t>(detail.candidateK);
    obj["chosen_k_valid"] = detail.chosenKValid;
    if (detail.chosenKValid)
        obj["chosen_k"] = static_cast<int64_t>(detail.chosenK);
    obj["rewrite_attempted"] = detail.rewriteAttempted;
    obj["rewrite_succeeded"] = detail.rewriteSucceeded;
    obj["rewrite_failure_reason"] = detail.rewriteFailureReason;
    obj["post_chunk_reclamp_attempted"] = detail.postChunkReclampAttempted;
    obj["post_chunk_reclamp_succeeded"] = detail.postChunkReclampSucceeded;
    obj["post_chunk_reclamp_applied"] = detail.postChunkReclampApplied;
    obj["post_chunk_max_k_valid"] = detail.postChunkMaxKValid;
    if (detail.postChunkMaxKValid)
        obj["post_chunk_max_k"] = static_cast<int64_t>(detail.postChunkMaxK);
    obj["post_chunk_iter_energy_valid"] = detail.postChunkIterEnergyValid;
    if (detail.postChunkIterEnergyValid)
        obj["post_chunk_iter_energy"] = detail.postChunkIterEnergy;
    obj["post_chunk_reclamp_error"] = detail.postChunkReclampError;
    return obj;
}

static json::Object functionSnapshotToJSON(const std::string &functionName,
                                           const LoopStripMiningFunctionSnapshot &snapshot) {
    json::Object summary;
    summary["loops_seen"] = static_cast<int64_t>(snapshot.loopsSeen);
    summary["loops_eligible"] = static_cast<int64_t>(snapshot.loopsEligible);
    summary["loops_rewritten"] = static_cast<int64_t>(snapshot.loopsRewritten);
    summary["loops_chunked"] = static_cast<int64_t>(snapshot.loopsChunked);

    json::Object skippedReasons;
    for (const auto &[reason, count] : snapshot.skippedReasons)
        skippedReasons[reason] = static_cast<int64_t>(count);

    json::Array chosenKValues;
    for (const auto &[header, k] : snapshot.chosenKByHeader) {
        json::Object item;
        item["loop_header"] = header;
        item["chosen_k"] = static_cast<int64_t>(k);
        chosenKValues.emplace_back(std::move(item));
    }

    std::vector<LoopStripMiningDetail> sortedDetails = snapshot.loopDetails;
    std::sort(sortedDetails.begin(), sortedDetails.end(),
              [](const LoopStripMiningDetail &lhs, const LoopStripMiningDetail &rhs) {
                  return lhs.loopHeader < rhs.loopHeader;
              });

    json::Array loopDetails;
    loopDetails.reserve(sortedDetails.size());
    for (const auto &detail : sortedDetails)
        loopDetails.emplace_back(loopDetailToJSON(detail));

    json::Object obj;
    obj["function"] = functionName;
    obj["summary"] = std::move(summary);
    obj["skipped_reasons"] = std::move(skippedReasons);
    obj["chosen_k_values"] = std::move(chosenKValues);
    obj["loop_details"] = std::move(loopDetails);
    return obj;
}

static std::map<std::string, LoopStripMiningFunctionSnapshot> &getLoopStripMiningStatsStore() {
    static std::map<std::string, LoopStripMiningFunctionSnapshot> store;
    return store;
}

static void writeLoopStripMiningStatsJSON(const Function &F, const LoopStripMiningStats &stats) {
    if (LoopStripMiningStatsJsonOpt.empty())
        return;

    auto &store = getLoopStripMiningStatsStore();
    store[F.getName().str()] = stats;

    json::Array functions;
    functions.reserve(store.size());
    for (const auto &[functionName, snapshot] : store)
        functions.emplace_back(functionSnapshotToJSON(functionName, snapshot));

    json::Object root;
    root["functions"] = std::move(functions);
    checkpoint::writeStatsJSON(LoopStripMiningStatsJsonOpt, std::move(root));
}

static std::optional<uint64_t> getConstantTripCount(Loop *L, ScalarEvolution &SE,
                                                    const BasicBlock *ExitingBlock);

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

static WorstCasePathResult
computeWorstCaseSummaryPathResult(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
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
        auto subPath = computeWorstCaseSummaryPathResult(SubL, blockEnergy, LI, SE);
        if (!subPath.ok) {
            result.error = "sub-loop-energy-unavailable";
            return result;
        }

        unsigned scevTC = SE.getSmallConstantTripCount(SubL);
        auto markerTC = getMarkerTripCount(SubL);
        uint64_t tc;
        if (scevTC > 0 && markerTC)
            tc = std::min<uint64_t>(scevTC, *markerTC);
        else if (scevTC > 0)
            tc = scevTC;
        else if (markerTC)
            tc = *markerTC;
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

static std::optional<uint64_t> getConstantTripCount(Loop *L, ScalarEvolution &SE,
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
        loopId = (F->getName() + "::" + checkpoint::getBlockName(*Header, *F)).str();
    }

    if (backedgeCount.getActiveBits() > 64) {
        PLOGW << "LoopStripMiningPass warning: backedge count for loop " << loopId
              << " exceeds 64 bits; skipping loop";
        return std::nullopt;
    }

    uint64_t backedgeValue = backedgeCount.getZExtValue();
    bool exitAtLatch = (ExitingBlock == L->getLoopLatch());
    if (exitAtLatch && backedgeValue == std::numeric_limits<uint64_t>::max()) {
        PLOGW << "LoopStripMiningPass warning: backedge count for loop " << loopId
              << " cannot be incremented safely; skipping loop";
        return std::nullopt;
    }

    // For loops exiting at the header, backedge count matches loop-body
    // iterations. For loops exiting at the latch, body iterations are one more.
    if (exitAtLatch) {
        return backedgeValue + 1;
    }
    return backedgeValue;
}

static bool supportsExitRewriteForm(Loop *L, uint64_t N) {
    if (!L) {
        return false;
    }

    BasicBlock *Preheader = L->getLoopPreheader();
    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    BasicBlock *ExitBlock = L->getExitBlock();
    BasicBlock *ExitingBB = L->getExitingBlock();
    if (!Preheader || !Header || !Latch || !ExitBlock || !ExitingBB) {
        return false;
    }

    PHINode *IV = L->getCanonicalInductionVariable();
    if (!IV) {
        return false;
    }

    auto *ExitBr = dyn_cast<BranchInst>(ExitingBB->getTerminator());
    if (!ExitBr || !ExitBr->isConditional()) {
        return false;
    }

    auto *ExitCmp = dyn_cast<ICmpInst>(ExitBr->getCondition());
    if (!ExitCmp) {
        return false;
    }

    BasicBlock *BackedgeBB = nullptr;
    BasicBlock *IncomingBB = nullptr;
    if (!L->getIncomingAndBackEdge(IncomingBB, BackedgeBB)) {
        return false;
    }

    Value *IVNext = IV->getIncomingValueForBlock(BackedgeBB);
    Value *CmpOp0 = ExitCmp->getOperand(0);
    Value *CmpOp1 = ExitCmp->getOperand(1);

    int boundOperandIdx = -1;
    bool compareUsesCurrentIV = false;
    if (CmpOp0 == IV || CmpOp0 == IVNext) {
        boundOperandIdx = 1;
        compareUsesCurrentIV = (CmpOp0 == IV);
    } else if (CmpOp1 == IV || CmpOp1 == IVNext) {
        boundOperandIdx = 0;
        compareUsesCurrentIV = (CmpOp1 == IV);
    } else {
        return false;
    }

    auto *OrigBound = dyn_cast<ConstantInt>(ExitCmp->getOperand(boundOperandIdx));
    if (!OrigBound) {
        return false;
    }

    bool exitAtLatch = (ExitingBB == Latch);
    uint64_t expectedBound = N;
    if (compareUsesCurrentIV && exitAtLatch) {
        if (N == 0) {
            return false;
        }
        expectedBound = N - 1;
    }

    return OrigBound->getZExtValue() == expectedBound;
}

using checkpoint::computeBoundaryStateMarginOnPath;

static double computeNvmAccessMarginOnPath(const SmallPtrSetImpl<const BasicBlock *> &pathBlocks,
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
            unsigned accesses = state.getLoadCount(BB, GV) + state.getStoreCount(BB, GV);
            nvmAccessMargin += static_cast<double>(accesses) * params.nvmAccessPenalty;
        }
        for (GlobalVariable *GV : ineligGlobals) {
            unsigned accesses = state.getLoadCount(BB, GV) + state.getStoreCount(BB, GV);
            nvmAccessMargin += static_cast<double>(accesses) * params.nvmAccessPenalty;
        }
    }
    return nvmAccessMargin;
}

/// Exact constant trip count for loops with a single exiting block; records
/// it in the plan detail when known.
static std::optional<uint64_t> computeExactTripCount(Loop *L, ScalarEvolution &SE,
                                                     PlanResult &result) {
    SmallVector<BasicBlock *, 8> exitingBlocks;
    L->getExitingBlocks(exitingBlocks);
    std::optional<uint64_t> tc;
    if (exitingBlocks.size() == 1)
        tc = getConstantTripCount(L, SE, exitingBlocks.front());
    if (tc) {
        result.detail.tripCountKnown = true;
        result.detail.tripCount = *tc;
    }
    return tc;
}

/// Tier 1: strip mining via exit rewrite. Requires a canonical IV, a single
/// exiting block, an exact trip count >= 2, and the supported exit form.
/// Returns true when \p result was finalized (plan or terminal skip); false
/// to fall through to the chunking tier.
static bool tryStripMiningPlan(Loop *L, uint64_t K, double iterEnergy,
                               std::optional<uint64_t> exactTripCount, PlanResult &result) {
    PHINode *IV = L->getCanonicalInductionVariable();
    SmallVector<BasicBlock *, 8> exitingBlocks;
    L->getExitingBlocks(exitingBlocks);
    if (!IV || exitingBlocks.size() != 1 || !exactTripCount ||
        !supportsExitRewriteForm(L, *exactTripCount) || *exactTripCount < 2)
        return false;

    if (K >= *exactTripCount) {
        result.skipReason = "k-covers-entire-loop";
        result.detail.skipReason = result.skipReason;
        return true;
    }
    LoopRewritePlan plan;
    plan.L = L;
    plan.N = *exactTripCount;
    plan.K = K;
    plan.iterEnergy = iterEnergy;
    plan.isChunking = false;
    result.plan = plan;
    result.skipReason.clear();
    result.detail.isChunking = false;
    result.detail.chosenKValid = true;
    result.detail.chosenK = K;
    return true;
}

/// Tier 2: chunking fallback. Only needs the latch to branch back to the
/// header. Finalizes \p result with either a chunking plan or a skip.
static void buildChunkingPlan(Loop *L, BasicBlock *Latch, uint64_t K, double iterEnergy,
                              std::optional<uint64_t> exactTripCount, PlanResult &result) {
    BasicBlock *Header = L->getHeader();
    auto *LatchBr = dyn_cast<BranchInst>(Latch->getTerminator());
    if (!LatchBr) {
        result.skipReason = "latch-not-branch-inst";
        result.detail.skipReason = result.skipReason;
        return;
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
        result.detail.skipReason = result.skipReason;
        return;
    }

    // If we can determine trip count and K covers it, skip
    std::optional<uint64_t> knownTC = exactTripCount;
    if (!knownTC)
        knownTC = getMarkerTripCount(L);
    if (knownTC) {
        result.detail.tripCountKnown = true;
        result.detail.tripCount = *knownTC;
    }
    if (knownTC && K >= *knownTC) {
        result.skipReason = "k-covers-entire-loop";
        result.detail.skipReason = result.skipReason;
        return;
    }

    LoopRewritePlan plan;
    plan.L = L;
    // Carry known upper-bound trip count into chunking so the generated
    // outer loop can be annotated with a conservative upper bound.
    plan.N = knownTC ? *knownTC : 0;
    plan.K = K;
    plan.iterEnergy = iterEnergy;
    plan.isChunking = true;
    result.plan = plan;
    result.skipReason.clear();
    result.detail.isChunking = true;
    result.detail.chosenKValid = true;
    result.detail.chosenK = K;
}

static PlanResult buildRewritePlan(Loop *L, ScalarEvolution &SE,
                                   const DenseMap<const BasicBlock *, double> &blockEnergy,
                                   const checkpoint::MILPEnergyParams &params, LoopInfo &LI,
                                   const checkpoint::StateAnalysis &state) {
    PlanResult result;
    result.skipReason = "unknown";
    result.detail = buildInitialLoopDetail(L);

    // ── Common checks (both strip mining and chunking) ──
    if (!L->isLoopSimplifyForm()) {
        result.skipReason = "not-loop-simplify-form";
        result.detail.skipReason = result.skipReason;
        return result;
    }
    if (!L->hasDedicatedExits()) {
        result.skipReason = "no-dedicated-exits";
        result.detail.skipReason = result.skipReason;
        return result;
    }
    if (!L->getLoopPreheader()) {
        result.skipReason = "missing-preheader";
        result.detail.skipReason = result.skipReason;
        return result;
    }
    BasicBlock *Latch = L->getLoopLatch();
    if (!Latch) {
        result.skipReason = "missing-single-latch";
        result.detail.skipReason = result.skipReason;
        return result;
    }
    if (containsInvoke(L)) {
        result.skipReason = "contains-invoke";
        result.detail.skipReason = result.skipReason;
        return result;
    }

    // ── Common: compute energy budget and K ──
    double budget = params.capacity - params.E_pro - params.E_epi;
    result.detail.budget = budget;
    if (budget <= 0.0) {
        result.skipReason = "nonpositive-energy-budget";
        result.skipDetail = "capacity=" + std::to_string(params.capacity) +
                            ", E_pro=" + std::to_string(params.E_pro) +
                            ", E_epi=" + std::to_string(params.E_epi) +
                            ", budget=" + std::to_string(budget);
        result.detail.skipReason = result.skipReason;
        result.detail.skipDetail = result.skipDetail;
        return result;
    }

    WorstCasePathResult iterEnergy = computeWorstCaseIterationEnergy(L, blockEnergy, LI, SE);
    if (!iterEnergy.ok) {
        result.skipReason = iterEnergy.error;
        result.detail.skipReason = result.skipReason;
        return result;
    }
    if (iterEnergy.energy <= 0.0) {
        result.skipReason = "nonpositive-iteration-energy";
        result.detail.skipReason = result.skipReason;
        return result;
    }
    result.detail.iterEnergy = iterEnergy.energy;

    double perIterNvmPenalty = computeNvmAccessMarginOnPath(iterEnergy.blocksOnPath, state, params);
    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = computeBoundaryStateMarginOnPath(
        iterEnergy.blocksOnPath, state, params, restoreLiveInMargin, commitDefMargin);
    double budgetAfterBoundary = budget - boundaryStateMargin;
    result.detail.perIterNvmPenalty = perIterNvmPenalty;
    result.detail.loopStripMiningCost = params.loopStripMiningCost;
    result.detail.restoreLiveInMargin = restoreLiveInMargin;
    result.detail.commitDefMargin = commitDefMargin;
    result.detail.boundaryStateMargin = boundaryStateMargin;
    result.detail.budgetAfterBoundary = budgetAfterBoundary;
    if (budgetAfterBoundary <= 0.0) {
        result.skipReason = "nonpositive-effective-budget";
        result.skipDetail =
            "budget=" + std::to_string(budget) +
            ", per-iter-nvm-penalty=" + std::to_string(perIterNvmPenalty) +
            ", loop-strip-mining-cost=" + std::to_string(params.loopStripMiningCost) +
            ", per-iter-path-energy=" + std::to_string(iterEnergy.energy) +
            ", restore-livein-margin=" + std::to_string(restoreLiveInMargin) +
            ", commit-def-margin=" + std::to_string(commitDefMargin) +
            ", boundary-state-margin=" + std::to_string(boundaryStateMargin) +
            ", budget-after-boundary=" + std::to_string(budgetAfterBoundary);
        result.detail.skipReason = result.skipReason;
        result.detail.skipDetail = result.skipDetail;
        return result;
    }

    double perIterTotalEnergy = iterEnergy.energy + perIterNvmPenalty + params.loopStripMiningCost;
    result.detail.perIterTotalEnergy = perIterTotalEnergy;
    if (perIterTotalEnergy <= 0.0) {
        result.skipReason = "nonpositive-per-iter-total-energy";
        result.skipDetail =
            "per-iter-path-energy=" + std::to_string(iterEnergy.energy) +
            ", per-iter-nvm-penalty=" + std::to_string(perIterNvmPenalty) +
            ", loop-strip-mining-cost=" + std::to_string(params.loopStripMiningCost) +
            ", per-iter-total-energy=" + std::to_string(perIterTotalEnergy);
        result.detail.skipReason = result.skipReason;
        result.detail.skipDetail = result.skipDetail;
        return result;
    }

    // Enforce strict inequality: K * perIterTotalEnergy < budgetAfterBoundary.
    double strictBudget =
        std::nextafter(budgetAfterBoundary, -std::numeric_limits<double>::infinity());
    double rawK = std::floor(strictBudget / perIterTotalEnergy);
    result.detail.strictBudget = strictBudget;
    result.detail.rawK = rawK;
    if (!std::isfinite(rawK) || rawK <= 0.0) {
        result.skipReason = "k-zero";
        result.skipDetail =
            "budget-after-boundary=" + std::to_string(budgetAfterBoundary) +
            ", strict-budget=" + std::to_string(strictBudget) +
            ", per-iter-path-energy=" + std::to_string(iterEnergy.energy) +
            ", per-iter-nvm-penalty=" + std::to_string(perIterNvmPenalty) +
            ", loop-strip-mining-cost=" + std::to_string(params.loopStripMiningCost) +
            ", per-iter-total-energy=" + std::to_string(perIterTotalEnergy);
        result.detail.skipReason = result.skipReason;
        result.detail.skipDetail = result.skipDetail;
        return result;
    }

    auto K = static_cast<uint64_t>(rawK);
    result.detail.candidateKValid = true;
    result.detail.candidateK = K;
    if (K <= 1) {
        result.skipReason = "k-not-beneficial";
        result.detail.skipReason = result.skipReason;
        return result;
    }
    if (K > std::numeric_limits<unsigned>::max()) {
        result.skipReason = "k-too-large";
        result.detail.skipReason = result.skipReason;
        return result;
    }

    // ── Tier 1: strip mining; Tier 2: chunking fallback ──
    std::optional<uint64_t> exactTripCount = computeExactTripCount(L, SE, result);
    if (tryStripMiningPlan(L, K, iterEnergy.energy, exactTripCount, result))
        return result;
    buildChunkingPlan(L, Latch, K, iterEnergy.energy, exactTripCount, result);
    return result;
}

static void refreshBlockEnergy(Function &F, checkpoint::EnergyEstimator &estimator,
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

static ChunkBudgetResult
recomputeChunkKWithOverhead(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
                            const checkpoint::MILPEnergyParams &params, LoopInfo &LI,
                            ScalarEvolution &SE, const checkpoint::StateAnalysis &state) {
    ChunkBudgetResult out;

    double budget = params.capacity - params.E_pro - params.E_epi;
    if (budget <= 0.0) {
        out.error = "nonpositive-energy-budget";
        return out;
    }

    WorstCasePathResult iterEnergy = computeWorstCaseIterationEnergy(L, blockEnergy, LI, SE);
    if (!iterEnergy.ok || iterEnergy.energy <= 0.0) {
        out.error =
            iterEnergy.error.empty() ? "post-chunk-iter-energy-unavailable" : iterEnergy.error;
        return out;
    }

    double perIterNvmPenalty = computeNvmAccessMarginOnPath(iterEnergy.blocksOnPath, state, params);
    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = computeBoundaryStateMarginOnPath(
        iterEnergy.blocksOnPath, state, params, restoreLiveInMargin, commitDefMargin);
    double budgetAfterBoundary = budget - boundaryStateMargin;
    if (budgetAfterBoundary <= 0.0) {
        out.error = "nonpositive-effective-budget";
        return out;
    }

    double perIterTotalEnergy = iterEnergy.energy + perIterNvmPenalty + params.loopStripMiningCost;
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

    auto maxK = static_cast<uint64_t>(rawK);
    if (maxK > std::numeric_limits<unsigned>::max()) {
        maxK = std::numeric_limits<unsigned>::max();
    }

    out.ok = true;
    out.maxK = maxK;
    out.iterEnergy = iterEnergy.energy;
    return out;
}

static ChunkBudgetResult
recomputeChunkKForSummaryBudget(Loop *L, const DenseMap<const BasicBlock *, double> &blockEnergy,
                                const checkpoint::MILPEnergyParams &params, LoopInfo &LI,
                                ScalarEvolution &SE, const checkpoint::StateAnalysis &state) {
    ChunkBudgetResult out;

    double budget = params.capacity - params.E_pro - params.E_epi;
    if (budget <= 0.0) {
        out.error = "nonpositive-energy-budget";
        return out;
    }

    WorstCasePathResult path = computeWorstCaseSummaryPathResult(L, blockEnergy, LI, SE);
    if (!path.ok || path.energy <= 0.0) {
        out.error = path.error.empty() ? "post-chunk-summary-path-unavailable" : path.error;
        return out;
    }

    double perIterNvmPenalty = 0.0;
    for (const BasicBlock *BB : path.blocksOnPath) {
        for (GlobalVariable *GV : state.getVMObjs()) {
            unsigned accesses = state.getLoadCount(BB, GV) + state.getStoreCount(BB, GV);
            perIterNvmPenalty += static_cast<double>(accesses) * params.nvmAccessPenalty;
        }
    }

    double restoreLiveInMargin = 0.0;
    double commitDefMargin = 0.0;
    double boundaryStateMargin = computeBoundaryStateMarginOnPath(
        path.blocksOnPath, state, params, restoreLiveInMargin, commitDefMargin);
    double budgetAfterBoundary = budget - boundaryStateMargin;
    if (budgetAfterBoundary <= 0.0) {
        out.error = "nonpositive-effective-budget";
        return out;
    }

    double perIterTotalEnergy = path.energy + perIterNvmPenalty;
    if (perIterTotalEnergy <= 0.0) {
        out.error = "nonpositive-per-iter-total-energy";
        return out;
    }

    double strictBudget =
        std::nextafter(budgetAfterBoundary, -std::numeric_limits<double>::infinity());
    double rawK = std::floor(strictBudget / perIterTotalEnergy);
    if (!std::isfinite(rawK) || rawK <= 0.0) {
        out.error = "post-chunk-k-zero";
        return out;
    }

    auto maxK = static_cast<uint64_t>(rawK);
    if (maxK > std::numeric_limits<unsigned>::max()) {
        maxK = std::numeric_limits<unsigned>::max();
    }

    out.ok = true;
    out.maxK = maxK;
    out.iterEnergy = path.energy;
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

static bool reclampExistingChunkedLoops(Function &F, LoopInfo &LI, ScalarEvolution &SE,
                                        AAResults &AA, checkpoint::EnergyEstimator &estimator,
                                        const checkpoint::MILPEnergyParams &params,
                                        DenseMap<const BasicBlock *, double> &blockEnergy,
                                        LoopStripMiningStats &stats) {
    checkpoint::CFGAnalysis cfg(F, LI, estimator);
    checkpoint::StateAnalysis state(F, AA, cfg);
    if (state.hasAnalysisErrors()) {
        state.printAnalysisErrors(errs());
        PLOGW << "LoopStripMiningPass: skipping re-clamp for " << F.getName()
              << " due to unresolved memory/call effects.";
        return false;
    }

    bool changed = false;
    std::function<void(Loop *)> visitLoop = [&](Loop *L) {
        if (!L)
            return;

        if (hasStripMinedLoopMetadata(L)) {
            stats.loopsSeen++;
            LoopStripMiningDetail detail = buildInitialLoopDetail(L);
            auto currentK = getMarkerTripCount(L);
            if (!currentK) {
                stats.skippedReasons["reclamp-missing-tripcount-metadata"]++;
                detail.decision = "skipped";
                detail.skipReason = "reclamp-missing-tripcount-metadata";
                detail.skipDetail = "strip-mined loop missing llvm.loop.tripcount.upper metadata";
                stats.loopDetails.push_back(detail);
            } else {
                detail.chosenKValid = true;
                detail.chosenK = *currentK;

                if (!isChunkCounterLoop(L)) {
                    stats.skippedReasons["reclamp-unsupported-loop-form"]++;
                    detail.decision = "skipped";
                    detail.skipReason = "reclamp-unsupported-loop-form";
                    detail.skipDetail = "strip-mined loop has no chunk.counter PHI";
                } else {
                    stats.loopsEligible++;
                    detail.postChunkReclampAttempted = true;

                    ChunkBudgetResult reclamp =
                        recomputeChunkKForSummaryBudget(L, blockEnergy, params, LI, SE, state);
                    if (!reclamp.ok) {
                        stats.skippedReasons["chunk-k-reclamp-unavailable"]++;
                        detail.decision = "kept";
                        detail.postChunkReclampError = reclamp.error;
                        PLOGW << "LoopStripMiningPass: post-energy chunk K re-clamp unavailable "
                              << F.getName() << "::" << detail.loopHeader
                              << " reason=" << reclamp.error << " current-K=" << *currentK;
                    } else {
                        detail.postChunkReclampSucceeded = true;
                        detail.postChunkMaxKValid = true;
                        detail.postChunkMaxK = reclamp.maxK;
                        detail.postChunkIterEnergyValid = true;
                        detail.postChunkIterEnergy = reclamp.iterEnergy;

                        uint64_t newK = std::min<uint64_t>(*currentK, reclamp.maxK);
                        if (newK != *currentK) {
                            if (!updateChunkLoopBound(L, newK)) {
                                stats.skippedReasons["chunk-k-reclamp-update-failed"]++;
                                detail.decision = "kept";
                                detail.postChunkReclampError = "chunk-k-reclamp-update-failed";
                                PLOGW << "LoopStripMiningPass: post-energy chunk K re-clamp "
                                      << "update failed " << F.getName()
                                      << "::" << detail.loopHeader << " current-K=" << *currentK
                                      << " new-K=" << newK;
                            } else {
                                setLoopTripCountMetadata(L, newK);
                                SE.forgetLoop(L);
                                detail.decision = "reclamped";
                                detail.postChunkReclampApplied = true;
                                detail.chosenK = newK;
                                stats.loopsRewritten++;
                                stats.loopsChunked++;
                                changed = true;
                                PLOGI << "LoopStripMiningPass: post-energy chunk K re-clamped "
                                      << F.getName() << "::" << detail.loopHeader
                                      << " current-K=" << *currentK << " new-K=" << newK
                                      << " E_iter_wc_post=" << reclamp.iterEnergy;
                            }
                        } else {
                            detail.decision = "kept";
                            PLOGD << "LoopStripMiningPass: post-energy chunk K unchanged "
                                  << F.getName() << "::" << detail.loopHeader << " K=" << *currentK
                                  << " E_iter_wc_post=" << reclamp.iterEnergy;
                        }
                    }
                }

                stats.chosenKByHeader.emplace_back(detail.loopHeader, detail.chosenK);
                stats.loopDetails.push_back(detail);
            }
        }

        for (Loop *SubL : L->getSubLoops())
            visitLoop(SubL);
    };

    for (Loop *L : LI)
        visitLoop(L);

    return changed;
}

static void selectInNest(Loop *L, ScalarEvolution &SE,
                         const DenseMap<const BasicBlock *, double> &blockEnergy,
                         const checkpoint::MILPEnergyParams &params, LoopInfo &LI,
                         const checkpoint::StateAnalysis &state, std::vector<SelectedLoopPlan> &out,
                         LoopStripMiningStats &stats) {

    stats.loopsSeen++;
    PlanResult pr = buildRewritePlan(L, SE, blockEnergy, params, LI, state);

    if (pr.plan) {
        pr.detail.decision = "selected";
        pr.detail.skipReason.clear();
        pr.detail.skipDetail.clear();
        size_t detailIndex = stats.loopDetails.size();
        stats.loopDetails.push_back(pr.detail);
        out.push_back({L, *pr.plan, detailIndex});
        return;
    }

    pr.detail.skipReason = pr.skipReason;
    pr.detail.skipDetail = pr.skipDetail;
    pr.detail.decision = (pr.skipReason == "k-covers-entire-loop") ? "fits-entirely" : "skipped";
    stats.loopDetails.push_back(pr.detail);

    {
        BasicBlock *Header = L->getHeader();
        const Function *F = Header ? Header->getParent() : nullptr;
        std::string headerName =
            (Header && F) ? checkpoint::getBlockName(*Header, *F) : "<unknown>";
        std::string funcName = F ? F->getName().str() : "<unknown>";
        std::string msg = "LoopStripMiningPass: skip " + funcName + "::" + headerName +
                          " reason=" + pr.skipReason;
        if (!pr.skipDetail.empty()) {
            msg += " " + pr.skipDetail;
        }
        PLOGD << msg;
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

static std::vector<SelectedLoopPlan>
selectLoopsToStripMine(LoopInfo &LI, ScalarEvolution &SE,
                       const DenseMap<const BasicBlock *, double> &blockEnergy,
                       const checkpoint::MILPEnergyParams &params,
                       const checkpoint::StateAnalysis &state, LoopStripMiningStats &stats) {
    std::vector<SelectedLoopPlan> selected;
    for (Loop *L : LI) {
        selectInNest(L, SE, blockEnergy, params, LI, state, selected, stats);
    }
    return selected;
}

static bool stripMineByExitRewrite(const LoopRewritePlan &plan, LoopInfo &LI, ScalarEvolution &SE,
                                   DominatorTree &DT, AssumptionCache &AC, AAResults &AA,
                                   const TargetTransformInfo &TTI) {
    // ── Phase 1: Extract and validate loop components ──
    Loop *L = plan.L;
    uint64_t N = plan.N, K = plan.K;

    BasicBlock *Preheader = L->getLoopPreheader();
    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    BasicBlock *ExitBlock = L->getExitBlock();
    BasicBlock *ExitingBB = L->getExitingBlock();
    Function *F = Header->getParent();
    LLVMContext &Ctx = F->getContext();

    if (!Preheader || !Header || !Latch || !ExitBlock || !ExitingBB)
        return false;

    PHINode *IV = L->getCanonicalInductionVariable();
    if (!IV)
        return false;
    Type *IVTy = IV->getType();

    // ── Phase 2: Find exit condition ──
    auto *ExitBr = dyn_cast<BranchInst>(ExitingBB->getTerminator());
    if (!ExitBr || !ExitBr->isConditional())
        return false;

    auto *ExitCmp = dyn_cast<ICmpInst>(ExitBr->getCondition());
    if (!ExitCmp)
        return false;

    Value *CmpOp0 = ExitCmp->getOperand(0);
    Value *CmpOp1 = ExitCmp->getOperand(1);
    BasicBlock *BackedgeBB = nullptr, *IncomingBB = nullptr;
    if (!L->getIncomingAndBackEdge(IncomingBB, BackedgeBB))
        return false;
    Value *IVNext = IV->getIncomingValueForBlock(BackedgeBB);

    int boundOperandIdx = -1;
    bool compareUsesCurrentIV = false;
    if (CmpOp0 == IV || CmpOp0 == IVNext) {
        boundOperandIdx = 1;
        compareUsesCurrentIV = (CmpOp0 == IV);
    } else if (CmpOp1 == IV || CmpOp1 == IVNext) {
        boundOperandIdx = 0;
        compareUsesCurrentIV = (CmpOp1 == IV);
    } else {
        return false;
    }

    auto *OrigBound = dyn_cast<ConstantInt>(ExitCmp->getOperand(boundOperandIdx));
    if (!OrigBound)
        return false;
    bool exitAtLatch = (ExitingBB == Latch);
    uint64_t expectedBound = N;
    if (compareUsesCurrentIV && exitAtLatch) {
        if (N == 0)
            return false;
        expectedBound = N - 1;
    }
    if (OrigBound->getZExtValue() != expectedBound)
        return false;

    // Collect LCSSA PHIs in ExitBlock before any modifications
    SmallVector<PHINode *, 4> lcssaPhis;
    for (PHINode &PN : ExitBlock->phis())
        lcssaPhis.push_back(&PN);

    // ── Phase 3: Create outer loop blocks ──
    BasicBlock *OuterHeader = BasicBlock::Create(Ctx, "outer.header", F, Header);
    BasicBlock *OuterLatch = BasicBlock::Create(Ctx, "outer.latch", F, ExitBlock);

    // ── Phase 4: Build OuterHeader ──
    IRBuilder<> OHB(OuterHeader);
    PHINode *OuterIV = OHB.CreatePHI(IVTy, 2, "outer.iv");

    // Forward non-IV Header PHIs through the outer loop
    SmallVector<HeaderPhiInfo, 4> headerPhiForwarding;
    for (PHINode &PN : Header->phis()) {
        if (&PN == IV)
            continue;
        Value *InitVal = PN.getIncomingValueForBlock(Preheader);
        PHINode *OHP = OHB.CreatePHI(PN.getType(), 2, PN.getName() + ".outer");
        headerPhiForwarding.push_back({&PN, OHP, InitVal});
    }

    // Inner limit: min(outer.iv + K, N)
    Value *OuterIVPlusK = OHB.CreateAdd(OuterIV, ConstantInt::get(IVTy, K), "outer.iv.plus.k");
    Value *NVal = ConstantInt::get(IVTy, N);
    Value *Cmp = OHB.CreateICmpULT(OuterIVPlusK, NVal, "min.cmp");
    Value *InnerLimit = OHB.CreateSelect(Cmp, OuterIVPlusK, NVal, "inner.limit");
    Value *InnerExitBound = InnerLimit;
    if (compareUsesCurrentIV && exitAtLatch) {
        // Latch-exiting loops that compare the current IV against N - 1 still
        // execute N iterations. Preserve that form so each chunk executes at
        // most K iterations rather than K + 1.
        InnerExitBound = OHB.CreateSub(InnerLimit, ConstantInt::get(IVTy, 1), "inner.exit.bound");
    }

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
    ExitCmp->setOperand(boundOperandIdx, InnerExitBound);
    ExitBr->replaceSuccessorWith(ExitBlock, OuterLatch);

    // ── Phase 7: Build OuterLatch ──
    IRBuilder<> OLB(OuterLatch);

    // Forwarding PHIs for LCSSA values escaping to ExitBlock
    SmallVector<PHINode *, 4> outerLatchPhis;
    for (PHINode *LCPhi : lcssaPhis) {
        Value *IncomingVal = LCPhi->getIncomingValueForBlock(ExitingBB);
        PHINode *OLP = OLB.CreatePHI(LCPhi->getType(), 1, LCPhi->getName() + ".ol");
        OLP->addIncoming(IncomingVal, ExitingBB);
        outerLatchPhis.push_back(OLP);
    }

    // Forwarding PHIs for non-IV Header PHIs (loop-carried state)
    SmallVector<PHINode *, 4> headerForwardPhis;
    for (auto &info : headerPhiForwarding) {
        Value *ForwardVal;
        if (ExitingBB == Header && Latch != Header) {
            // Multi-block loop exiting from header: the exit fires before the
            // body runs, so the loop-carried value (defined in the latch) does
            // not dominate the exit edge.  The header PHI itself does dominate
            // and already holds the last completed iteration's result.
            ForwardVal = info.headerPhi;
        } else {
            // Single-block loop (Latch == Header) or latch-exiting: the body
            // has executed, so forward the loop-carried value — the result
            // of the current (last) iteration.
            ForwardVal = info.headerPhi->getIncomingValueForBlock(Latch);
        }
        PHINode *FP =
            OLB.CreatePHI(info.headerPhi->getType(), 1, info.headerPhi->getName() + ".fwd");
        FP->addIncoming(ForwardVal, ExitingBB);
        headerForwardPhis.push_back(FP);
    }

    // Next outer IV
    Value *NextOuterIV = OLB.CreateAdd(OuterIV, ConstantInt::get(IVTy, K), "outer.iv.next");

    // Outer exit condition: outer runs ceil(N/K) times
    uint64_t outerTripCount = (N + K - 1) / K;
    Value *OuterContinue = OLB.CreateICmpULT(NextOuterIV, ConstantInt::get(IVTy, N), "outer.cmp");
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
            if (*I == L) {
                ParentLoop->removeChildLoop(I);
                break;
            }
        }
        ParentLoop->addChildLoop(OuterLoop);
    } else {
        for (auto I = LI.begin(); I != LI.end(); ++I) {
            if (*I == L) {
                LI.removeLoop(I);
                break;
            }
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

static bool stripMineByChunkCounter(const LoopRewritePlan &plan, LoopInfo &LI, ScalarEvolution &SE,
                                    DominatorTree &DT) {
    // ── Phase 1: Extract loop components ──
    Loop *L = plan.L;
    uint64_t K = plan.K;
    uint64_t NUpper = plan.N;

    BasicBlock *Preheader = L->getLoopPreheader();
    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    Function *F = Header->getParent();
    LLVMContext &Ctx = F->getContext();

    if (!Preheader || !Header || !Latch)
        return false;

    // ── Phase 2: Find latch backedge index (which successor == Header) ──
    auto *LatchBr = dyn_cast<BranchInst>(Latch->getTerminator());
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
    BasicBlock *OuterHeader = BasicBlock::Create(Ctx, "outer.header", F, Header);
    BasicBlock *CounterCheck = BasicBlock::Create(Ctx, "counter.check", F);
    BasicBlock *OuterLatch = BasicBlock::Create(Ctx, "outer.latch", F);

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
    // Use the target's pointer-sized integer for the counter.  Using i64 on
    // 16-bit targets (MSP430) generates broken multi-word arithmetic in the
    // LLVM backend, causing the counter comparison to never succeed.
    const DataLayout &DL = F->getParent()->getDataLayout();
    Type *CtrTy = DL.getIntPtrType(Ctx);
    PHINode *ChunkCounter = PHINode::Create(CtrTy, 2, "chunk.counter", Header->getFirstNonPHIIt());

    // ── Phase 7: Redirect latch backedge: Latch → CounterCheck ──
    LatchBr->setSuccessor(backedgeIdx, CounterCheck);

    // ── Phase 8: Build counter.check — increment counter, branch ──
    IRBuilder<> CCB(CounterCheck);
    Value *CounterNext = CCB.CreateAdd(ChunkCounter, ConstantInt::get(CtrTy, 1), "counter.next");
    Value *CounterDone = CCB.CreateICmpEQ(CounterNext, ConstantInt::get(CtrTy, K), "counter.done");
    CCB.CreateCondBr(CounterDone, OuterLatch, Header);

    // ── Phase 9: Update header PHIs — incoming block Latch → CounterCheck ──
    for (auto &info : headerPhiForwarding) {
        int idx = info.headerPhi->getBasicBlockIndex(Latch);
        if (idx >= 0) {
            info.headerPhi->setIncomingBlock(idx, CounterCheck);
        }
    }

    // Complete chunk counter PHI
    ChunkCounter->addIncoming(ConstantInt::get(CtrTy, 0), OuterHeader);
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
            if (*I == L) {
                ParentLoop->removeChildLoop(I);
                break;
            }
        }
        ParentLoop->addChildLoop(OuterLoop);
    } else {
        for (auto I = LI.begin(); I != LI.end(); ++I) {
            if (*I == L) {
                LI.removeLoop(I);
                break;
            }
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
    PLOGI << "=== Loop Strip-Mining: " << F.getName() << " ===";
    PLOGI << "  Loops considered:                " << stats.loopsSeen;
    PLOGI << "  Eligible loops:                  " << stats.loopsEligible;
    PLOGI << "  Rewritten loops:                 " << stats.loopsRewritten;
    PLOGI << "    Strip-mined:                   " << (stats.loopsRewritten - stats.loopsChunked);
    PLOGI << "    Chunked:                       " << stats.loopsChunked;

    unsigned skippedTotal = 0;
    for (const auto &entry : stats.skippedReasons) {
        skippedTotal += entry.second;
    }
    PLOGI << "  Skipped loops:                   " << skippedTotal;

    if (!stats.skippedReasons.empty()) {
        PLOGI << "  Skipped reason histogram:";
        for (const auto &entry : stats.skippedReasons) {
            PLOGI << "    - " << entry.first << ": " << entry.second;
        }
    }

    if (!stats.chosenKByHeader.empty()) {
        PLOGI << "  Chosen K values:";
        for (const auto &[header, k] : stats.chosenKByHeader) {
            PLOGI << "    - " << header << ": K=" << k;
        }
    }
}

} // anonymous namespace

namespace checkpoint {

PreservedAnalyses LoopStripMiningPass::run(Function &F, FunctionAnalysisManager &AM) {
    checkpoint::initLogging();

    if (F.isDeclaration()) {
        return PreservedAnalyses::all();
    }

    // Skip benchmark infrastructure functions — same filter as MILPCheckpointPass.
    StringRef name = F.getName();
    if (name.starts_with("timing_gpio") || name.starts_with("_timing_delay") ||
        name.starts_with("debug_") || name.starts_with("uart_")) {
        PLOGD << "LoopStripMiningPass: skipping benchmark infrastructure function " << name;
        return PreservedAnalyses::all();
    }

    if (MILPConfigOpt.getValue().empty()) {
        if (LoopStripMiningEnabledOpt) {
            PLOGE << "LoopStripMiningPass: missing -milp-config; skipping " << F.getName();
        }
        return PreservedAnalyses::all();
    }

    auto milpParamsOpt = parseMILPEnergyParams(MILPConfigOpt.getValue());
    if (!milpParamsOpt) {
        PLOGE << "LoopStripMiningPass: failed to parse MILP config for " << F.getName()
              << "; skipping";
        return PreservedAnalyses::all();
    }
    bool loopStripMiningEnabled =
        LoopStripMiningEnabledOpt.getValue() || milpParamsOpt->loopStripMiningEnabled;
    if (!loopStripMiningEnabled) {
        return PreservedAnalyses::all();
    }

    if (EnergyConfigOpt.getValue().empty()) {
        PLOGE << "LoopStripMiningPass: missing -energy-config; skipping " << F.getName();
        return PreservedAnalyses::all();
    }

    auto factory = EnergyEstimatorFactory::createDefault();
    std::unique_ptr<EnergyEstimator> estimator =
        factory.createFromConfig(EnergyConfigOpt.getValue());
    if (!estimator) {
        PLOGE << "LoopStripMiningPass: failed to create estimator for " << F.getName()
              << "; skipping";
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
    auto &AA = AM.getResult<AAManager>(F);
    LoopStripMiningStats stats;
    if (reclampOnly_) {
        bool changed = reclampExistingChunkedLoops(F, LI, SE, AA, *estimator, *milpParamsOpt,
                                                   blockEnergy, stats);
        writeLoopStripMiningStatsJSON(F, stats);
        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }

    auto &AC = AM.getResult<AssumptionAnalysis>(F);
    auto &TTI = AM.getResult<TargetIRAnalysis>(F);
    checkpoint::CFGAnalysis cfg(F, LI, *estimator);
    checkpoint::StateAnalysis state(F, AA, cfg);
    if (state.hasAnalysisErrors()) {
        state.printAnalysisErrors(errs());
        PLOGW << "LoopStripMiningPass: skipping " << F.getName()
              << " due to unresolved memory/call effects.";
        return PreservedAnalyses::all();
    }

    bool changed = false;

    // Select loops to strip-mine (outermost-first within each nest).
    // Snapshot headers as WeakTrackingVH so we can detect invalidation
    // from prior rewrites that may restructure the CFG.
    auto selected = selectLoopsToStripMine(LI, SE, blockEnergy, *milpParamsOpt, state, stats);
    struct LoopWorkItem {
        WeakTrackingVH headerHandle;
        LoopRewritePlan plan;
        size_t detailIndex = 0;
    };
    std::vector<LoopWorkItem> worklist;
    worklist.reserve(selected.size());
    for (const auto &selectedPlan : selected) {
        worklist.push_back({WeakTrackingVH(selectedPlan.L->getHeader()), selectedPlan.plan,
                            selectedPlan.detailIndex});
    }

    for (auto &item : worklist) {
        auto &detail = stats.loopDetails[item.detailIndex];
        auto *Header = dyn_cast_or_null<BasicBlock>(item.headerHandle);
        if (!Header) {
            stats.skippedReasons["loop-header-erased-after-prior-rewrite"]++;
            detail.rewriteAttempted = true;
            detail.rewriteFailureReason = "loop-header-erased-after-prior-rewrite";
            continue;
        }

        std::string headerName = checkpoint::getBlockName(*Header, F);
        Loop *L = LI.getLoopFor(Header);
        if (!L || L->getHeader() != Header) {
            stats.skippedReasons["loop-unresolvable-from-header"]++;
            detail.rewriteAttempted = true;
            detail.rewriteFailureReason = "loop-unresolvable-from-header";
            continue;
        }

        item.plan.L = L;
        stats.loopsEligible++;
        detail.rewriteAttempted = true;

        bool rewritten = item.plan.isChunking
                             ? stripMineByChunkCounter(item.plan, LI, SE, DT)
                             : stripMineByExitRewrite(item.plan, LI, SE, DT, AC, AA, TTI);
        if (!rewritten) {
            stats.skippedReasons["rewrite-utility-failed"]++;
            detail.rewriteFailureReason = "rewrite-utility-failed";
            PLOGW << "LoopStripMiningPass: rewrite failed " << F.getName() << "::" << headerName
                  << " K=" << item.plan.K;
            continue;
        }

        if (item.plan.isChunking) {
            // Re-estimate on the transformed loop to include chunking overhead
            // (counter.check block) and clamp K in place if needed.
            refreshBlockEnergy(F, *estimator, blockEnergy);
            checkpoint::CFGAnalysis postCfg(F, LI, *estimator);
            checkpoint::StateAnalysis postState(F, AA, postCfg);
            const checkpoint::StateAnalysis *reclampState = &state;
            if (postState.hasAnalysisErrors()) {
                PLOGW << "LoopStripMiningPass: post-chunk state analysis "
                      << "failed for re-clamp in " << F.getName()
                      << "; using pre-rewrite state margins";
            } else {
                reclampState = &postState;
            }
            detail.postChunkReclampAttempted = true;
            ChunkBudgetResult reclamp =
                recomputeChunkKWithOverhead(L, blockEnergy, *milpParamsOpt, LI, SE, *reclampState);
            if (!reclamp.ok) {
                stats.skippedReasons["chunk-k-reclamp-unavailable"]++;
                detail.postChunkReclampError = reclamp.error;
                PLOGW << "LoopStripMiningPass: chunk K re-clamp unavailable " << F.getName()
                      << "::" << headerName << " reason=" << reclamp.error
                      << " original-K=" << item.plan.K;
            } else {
                detail.postChunkReclampSucceeded = true;
                detail.postChunkMaxKValid = true;
                detail.postChunkMaxK = reclamp.maxK;
                detail.postChunkIterEnergyValid = true;
                detail.postChunkIterEnergy = reclamp.iterEnergy;
                item.plan.iterEnergy = reclamp.iterEnergy;
                uint64_t newK = std::min<uint64_t>(item.plan.K, reclamp.maxK);
                if (newK != item.plan.K) {
                    if (!updateChunkLoopBound(L, newK)) {
                        stats.skippedReasons["chunk-k-reclamp-update-failed"]++;
                        detail.postChunkReclampError = "chunk-k-reclamp-update-failed";
                        PLOGW << "LoopStripMiningPass: chunk K re-clamp update failed "
                              << F.getName() << "::" << headerName << " original-K=" << item.plan.K
                              << " new-K=" << newK;
                    } else {
                        setLoopTripCountMetadata(L, newK);
                        SE.forgetLoop(L);
                        detail.postChunkReclampApplied = true;
                        PLOGD << "LoopStripMiningPass: chunk K re-clamped " << F.getName()
                              << "::" << headerName << " original-K=" << item.plan.K
                              << " new-K=" << newK << " E_iter_wc_post=" << reclamp.iterEnergy;
                        item.plan.K = newK;
                    }
                }
            }
        }

        changed = true;
        stats.loopsRewritten++;
        if (item.plan.isChunking)
            stats.loopsChunked++;
        detail.chosenKValid = true;
        detail.chosenK = item.plan.K;
        detail.rewriteSucceeded = true;
        stats.chosenKByHeader.emplace_back(headerName, item.plan.K);
        PLOGD << "LoopStripMiningPass: " << (item.plan.isChunking ? "chunked " : "rewritten ")
              << F.getName() << "::" << headerName << " N=" << item.plan.N << " K=" << item.plan.K
              << " E_iter_wc=" << item.plan.iterEnergy;
    }

    if (changed && verifyFunction(F, &errs())) {
        report_fatal_error(Twine("LoopStripMiningPass: verifier reported errors in ") +
                               F.getName() + "; aborting instead of passing broken IR downstream",
                           /*gen_crash_diag=*/false);
    }

    printSummary(F, stats);
    writeLoopStripMiningStatsJSON(F, stats);
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace checkpoint
