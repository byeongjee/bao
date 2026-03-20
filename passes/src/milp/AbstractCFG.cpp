#include "milp/AbstractCFG.h"

#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"
#include "common/Logger.h"
#include "common/LoopTripCount.h"
#include "common/LoopUtils.h"
#include "milp/EnergyModel.h"
#include "milp/EnergyPathUtils.h"
#include "milp/StateAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace checkpoint {

namespace {

struct LoopAggregate {
    std::string nodeName;
    const BasicBlock *headerBB = nullptr;
    SmallPtrSet<const BasicBlock *, 16> loopBlocks;
    SmallPtrSet<const BasicBlock *, 16> pathBlocks;
    double pathEnergy = 0.0;
    std::map<llvm::GlobalVariable *, double> eNvmByGV;
    std::set<llvm::GlobalVariable *> eligLiveIn;
    std::set<llvm::GlobalVariable *> eligDefGlobals;
    std::set<llvm::Value *> ineligLiveIn;
    std::set<llvm::Value *> ineligDefVars;
    double fEntry = 1.0;
};

enum class VisitState {
    Unvisited = 0,
    Visiting,
    Visited,
};

static void collectOutermostFirst(Loop *L, std::vector<Loop *> &out) {
    out.push_back(L);
    for (Loop *Sub : L->getSubLoops()) {
        collectOutermostFirst(Sub, out);
    }
}

static std::vector<Loop *> collectOutermostFirst(LoopInfo &LI) {
    std::vector<Loop *> loops;
    for (Loop *L : LI) {
        collectOutermostFirst(L, loops);
    }
    return loops;
}

static WorstCasePathResult
computeWorstCaseWorstCasePathResult(Loop *L,
                                    const DenseMap<const BasicBlock *, double> &blockEnergyByBB,
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
    DenseMap<const Loop *, SmallPtrSet<const BasicBlock *, 16>> subLoopBlocks;
    for (Loop *SubL : L->getSubLoops()) {
        auto subPath = computeWorstCaseWorstCasePathResult(SubL, blockEnergyByBB, LI, SE);
        if (!subPath.ok) {
            result.error = "sub-loop-energy-unavailable";
            return result;
        }
        unsigned scevTC = SE.getSmallConstantTripCount(SubL);
        auto markerTC = getMarkerTripCount(SubL);
        unsigned tc;
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

    // Step 2: Block energy with sub-loop collapsing — inner-loop headers
    // carry collapsed total energy; other blocks use per-block energy.
    auto getEnergy = [&](const BasicBlock *BB) -> double {
        Loop *ChildL = getDirectChildLoop(L, BB, LI);
        if (ChildL && ChildL->getHeader() == BB) {
            auto it = subLoopTotal.find(ChildL);
            return (it != subLoopTotal.end()) ? it->second : 0.0;
        }
        auto it = blockEnergyByBB.find(BB);
        return (it != blockEnergyByBB.end()) ? it->second : 0.0;
    };

    // Step 3: Successor computation with sub-loop collapsing — inner-loop
    // headers jump directly to exit blocks, skipping inner-loop bodies.
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
        // If this block is an inner-loop header, expand pathBlocks to
        // include all blocks in that sub-loop for NVM/def/liveIn aggregation.
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

static bool overlapsSelected(Loop *L, const SmallPtrSetImpl<const BasicBlock *> &selectedBlocks) {
    for (const BasicBlock *BB : L->blocks()) {
        if (selectedBlocks.count(BB)) {
            return true;
        }
    }
    return false;
}

static std::string makeUniqueSummaryNodeName(const std::string &headerName,
                                             const std::set<std::string> &used) {
    std::string base = "__loop_summary__" + headerName;
    if (!used.count(base)) {
        return base;
    }
    for (unsigned i = 1; i < std::numeric_limits<unsigned>::max(); ++i) {
        std::string candidate = base + "_" + std::to_string(i);
        if (!used.count(candidate)) {
            return candidate;
        }
    }
    return base + "_overflow";
}

} // namespace

double AbstractCFG::getBlockEnergyCost(NodeId block) const {
    auto it = blockEnergyCost_.find(block);
    if (it != blockEnergyCost_.end()) {
        return it->second;
    }
    return 0.0;
}

const std::string &AbstractCFG::getNodeName(NodeId node) const {
    static const std::string kUnknown = "<unknown-node>";
    auto it = nodeNames_.find(node);
    if (it == nodeNames_.end()) {
        return kUnknown;
    }
    return it->second;
}

const std::set<llvm::GlobalVariable *> &AbstractCFG::getEligLiveIn(NodeId block) const {
    auto it = eligLiveIn_.find(block);
    if (it != eligLiveIn_.end()) {
        return it->second;
    }
    static const std::set<llvm::GlobalVariable *> kEmpty;
    return kEmpty;
}

bool AbstractCFG::getEligDefIndicator(NodeId block, llvm::GlobalVariable *gv) const {
    auto it = eligDefIndicator_.find(std::make_pair(block, gv));
    if (it != eligDefIndicator_.end()) {
        return it->second;
    }
    return false;
}

const std::vector<llvm::Value *> &AbstractCFG::getIneligibleObjs() const {
    return ineligibleObjs_;
}

bool AbstractCFG::isIneligible(llvm::Value *v) const {
    return ineligibleObjSet_.count(v) > 0;
}

const std::set<llvm::Value *> &AbstractCFG::getIneligLiveIn(NodeId block) const {
    auto it = ineligLiveIn_.find(block);
    if (it != ineligLiveIn_.end()) {
        return it->second;
    }
    static const std::set<llvm::Value *> kEmpty;
    return kEmpty;
}

bool AbstractCFG::getIneligDefIndicator(NodeId block, llvm::Value *v) const {
    auto it = ineligDefIndicator_.find(std::make_pair(block, v));
    if (it != ineligDefIndicator_.end()) {
        return it->second;
    }
    return false;
}

int AbstractCFG::getVarSizeBytes(llvm::Value *v) const {
    auto it = varSizeBytes_.find(v);
    if (it != varSizeBytes_.end()) {
        return it->second;
    }
    return 0;
}

double AbstractCFG::getEBase(NodeId block) const {
    auto it = blockEnergyCost_.find(block);
    if (it != blockEnergyCost_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getENvm(NodeId block, llvm::GlobalVariable *gv) const {
    auto it = eNvm_.find(std::make_pair(block, gv));
    if (it != eNvm_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getESave(llvm::Value *v) const {
    auto it = eSaveByVar_.find(v);
    if (it != eSaveByVar_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getERestore(llvm::Value *v) const {
    auto it = eRestoreByVar_.find(v);
    if (it != eRestoreByVar_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getFEntry(NodeId block) const {
    auto it = fEntry_.find(block);
    if (it != fEntry_.end()) {
        return it->second;
    }
    return 1.0;
}

double AbstractCFG::getFBoundary(NodeId block) const {
    auto it = fBoundary_.find(block);
    if (it != fBoundary_.end()) {
        return it->second;
    }
    return 1.0;
}

double AbstractCFG::getQReboot() const {
    return 1.0; // Always 1.0 — hardcoded after config unification
}

AbstractCFGBuildResult buildAbstractCFG(llvm::Function &F, llvm::LoopInfo &LI,
                                        llvm::ScalarEvolution &SE, const CFGAnalysis &cfg,
                                        const StateAnalysis &state, const EnergyModel &energy) {
    AbstractCFGBuildResult out;
    out.model = std::make_unique<AbstractCFG>();
    AbstractCFG &model = *out.model;

    model.params_ = energy.getParams();

    // Stage 1: Copy eligible globals and their sizes/costs.
    model.vmObjs_ = state.getVMObjs();
    for (llvm::GlobalVariable *GV : model.vmObjs_) {
        model.varSizeBytes_[GV] = static_cast<int>(state.getVarSizeBytes(GV));
        model.eSaveByVar_[GV] = energy.getESave(GV);
        model.eRestoreByVar_[GV] = energy.getERestore(GV);
    }

    // Stage 1b: Copy ineligible objects (Value*) and their sizes/costs.
    model.ineligibleObjs_ = state.getIneligibleObjs();
    for (llvm::Value *V : model.ineligibleObjs_) {
        model.ineligibleObjSet_.insert(V);
        model.varSizeBytes_[V] = static_cast<int>(state.getVarSizeBytes(V));
        model.eSaveByVar_[V] = energy.getESave(V);
        model.eRestoreByVar_[V] = energy.getERestore(V);
    }
    // Build per-BB energy map from CFGAnalysis.
    DenseMap<const BasicBlock *, double> blockEnergyByBB;
    std::set<std::string> usedNodeNames;
    for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
        blockEnergyByBB[BB] = cfg.getBlockInfo(BB).energyCost;
        usedNodeNames.insert(cfg.getBlockInfo(BB).name);
    }

    std::vector<Loop *> loops = collectOutermostFirst(LI);
    SmallPtrSet<const BasicBlock *, 32> summarizedConcreteBlocks;
    // summaryNodeName -> LoopAggregate
    std::map<std::string, LoopAggregate> summariesByNode;

    const double budget = model.params_.capacity - model.params_.E_pro - model.params_.E_epi;

    for (Loop *L : loops) {
        out.stats.loopsSeen++;
        BasicBlock *loopHeader = L->getHeader();
        std::string loopHeaderName = loopHeader ? getBlockName(*loopHeader, F) : "<unknown>";
        bool isStripMined = hasStripMinedLoopMetadata(L);
        if (isStripMined)
            out.stats.stripMinedLoopsSeen++;

        auto skipLoop = [&](const std::string &reason, const std::string &details = "") {
            out.stats.skippedReasons[reason]++;
            if (details.empty()) {
                PLOGD << "AbstractCFG skip " << F.getName() << "::" << loopHeaderName
                      << " reason=" << reason;
            } else {
                PLOGD << "AbstractCFG skip " << F.getName() << "::" << loopHeaderName
                      << " reason=" << reason << " details=" << details;
            }
            if (isStripMined) {
                out.stats.stripMinedLoopsSkipped++;
                if (details.empty()) {
                    PLOGW << "AbstractCFG warning: strip-mined loop not summarized " << F.getName()
                          << "::" << loopHeaderName << " reason=" << reason;
                } else {
                    PLOGW << "AbstractCFG warning: strip-mined loop not summarized " << F.getName()
                          << "::" << loopHeaderName << " reason=" << reason
                          << " details=" << details;
                }
            }
        };

        if (budget <= 0.0) {
            skipLoop("nonpositive-energy-budget",
                     "capacity=" + std::to_string(model.params_.capacity) +
                         ", E_pro=" + std::to_string(model.params_.E_pro) +
                         ", E_epi=" + std::to_string(model.params_.E_epi) +
                         ", budget=" + std::to_string(budget));
            continue;
        }
        if (!loopHeader || !L->getLoopLatch()) {
            skipLoop("missing-header-or-latch",
                     "has-header=" + std::string(loopHeader ? "true" : "false") +
                         ", has-latch=" + std::string(L->getLoopLatch() ? "true" : "false"));
            continue;
        }
        if (containsInvoke(L)) {
            skipLoop("contains-invoke");
            continue;
        }
        if (overlapsSelected(L, summarizedConcreteBlocks)) {
            skipLoop("overlaps-summarized-loop");
            continue;
        }

        WorstCasePathResult path = computeWorstCaseWorstCasePathResult(L, blockEnergyByBB, LI, SE);
        if (!path.ok) {
            skipLoop(path.error.empty() ? "unknown-path-summary-error" : path.error,
                     "path-energy-unavailable");
            continue;
        }
        if (path.energy <= 0.0) {
            skipLoop("nonpositive-loop-energy", "path-energy=" + std::to_string(path.energy));
            continue;
        }

        out.stats.loopsEligible++;

        // Compute loop trip count for total energy check.
        unsigned scevTC = SE.getSmallConstantTripCount(L);
        auto markerTC = getMarkerTripCount(L);
        unsigned loopTC;
        if (scevTC > 0 && markerTC)
            loopTC = std::min(scevTC, static_cast<unsigned>(*markerTC));
        else if (scevTC > 0)
            loopTC = scevTC;
        else if (markerTC)
            loopTC = static_cast<unsigned>(*markerTC);
        else {
            skipLoop("unknown-loop-trip-count",
                     "scev-trip-count=" + std::to_string(scevTC) + ", marker-trip-count=none");
            continue;
        }

        // Compute per-iteration NVM penalty on the selected path.
        double perIterNvmPenalty = 0.0;
        for (const BasicBlock *BB : path.blocksOnPath) {
            for (llvm::GlobalVariable *GV : model.vmObjs_) {
                perIterNvmPenalty += energy.getENvm(BB, GV);
            }
        }

        double restoreLiveInMargin = 0.0;
        double commitDefMargin = 0.0;
        double boundaryStateMargin = computeBoundaryStateMarginOnPath(
            path.blocksOnPath, state, model.params_, restoreLiveInMargin, commitDefMargin);

        const double budgetAfterBoundary = budget - boundaryStateMargin;
        double totalBaseEnergy = path.energy * static_cast<double>(loopTC);
        double totalNvmPenalty = perIterNvmPenalty * static_cast<double>(loopTC);
        double totalEnergyWithNvm = totalBaseEnergy + totalNvmPenalty;
        if (budgetAfterBoundary <= 0.0 || !(totalEnergyWithNvm < budgetAfterBoundary)) {
            skipLoop("loop-total-exceeds-budget",
                     "path-energy=" + std::to_string(path.energy) +
                         ", per-iter-path-energy=" + std::to_string(path.energy) +
                         ", per-iter-nvm-penalty=" + std::to_string(perIterNvmPenalty) +
                         ", trip-count=" + std::to_string(loopTC) +
                         ", total-base-energy=" + std::to_string(totalBaseEnergy) +
                         ", total-nvm-penalty=" + std::to_string(totalNvmPenalty) +
                         ", total-energy-with-nvm=" + std::to_string(totalEnergyWithNvm) +
                         ", nvm-access-margin=" + std::to_string(perIterNvmPenalty) +
                         ", restore-livein-margin=" + std::to_string(restoreLiveInMargin) +
                         ", commit-def-margin=" + std::to_string(commitDefMargin) +
                         ", boundary-state-margin=" + std::to_string(boundaryStateMargin) +
                         ", budget-after-boundary=" + std::to_string(budgetAfterBoundary));
            continue;
        }

        BasicBlock *headerBB = loopHeader;
        std::string headerName = loopHeaderName;
        std::string nodeName = makeUniqueSummaryNodeName(headerName, usedNodeNames);
        usedNodeNames.insert(nodeName);

        LoopAggregate agg;
        agg.nodeName = nodeName;
        agg.headerBB = headerBB;
        agg.pathBlocks = std::move(path.blocksOnPath);
        agg.pathEnergy = totalBaseEnergy;
        // Summary represents all iterations — entered once per loop invocation.
        if (BasicBlock *PH = L->getLoopPreheader()) {
            agg.fEntry = energy.getFEntry(PH);
        } else {
            agg.fEntry = energy.getFEntry(headerBB);
        }

        for (const BasicBlock *BB : L->blocks()) {
            agg.loopBlocks.insert(BB);
            summarizedConcreteBlocks.insert(BB);
        }

        // Aggregate eligible globals across loop blocks.
        auto aggregateEligGV = [&](llvm::GlobalVariable *GV) {
            double nvmSum = 0.0;
            for (const BasicBlock *BB : agg.pathBlocks) {
                nvmSum += energy.getENvm(BB, GV);
            }

            bool hasDef = false;
            bool hasLiveIn = false;
            for (const BasicBlock *BB : agg.loopBlocks) {
                hasDef |= state.getEligDefIndicator(BB, GV);
                hasLiveIn |= state.getEligLiveIn(BB).count(GV) > 0;
            }

            if (nvmSum != 0.0) {
                agg.eNvmByGV[GV] = nvmSum * static_cast<double>(loopTC);
            }
            if (hasDef) {
                agg.eligDefGlobals.insert(GV);
            }
            if (hasLiveIn) {
                agg.eligLiveIn.insert(GV);
            }
        };

        for (llvm::GlobalVariable *GV : model.vmObjs_)
            aggregateEligGV(GV);

        // Aggregate ineligible objects across all loop blocks.
        auto aggregateIneligVar = [&](llvm::Value *V) {
            bool hasDef = false;
            bool hasLiveIn = false;

            for (const BasicBlock *BB : agg.loopBlocks) {
                hasDef |= state.getIneligDefIndicator(BB, V);
                hasLiveIn |= state.getIneligLiveIn(BB).count(V) > 0;
            }

            if (hasDef) {
                agg.ineligDefVars.insert(V);
            }
            if (hasLiveIn) {
                // SSA values have a single definition.  If that def is inside
                // the loop, the value is loop-internal: used across blocks
                // within the loop but not entering from outside.  Exclude it
                // from the summary's live-in so the instrumenter doesn't emit
                // a spurious restore that overwrites the fresh computation.
                // Allocas/globals use read-before-store liveness, where live-in
                // at one block and def at another are independent — keep those.
                bool isSSA = !llvm::isa<llvm::AllocaInst>(V) && !llvm::isa<llvm::GlobalVariable>(V);
                if (!isSSA || !hasDef)
                    agg.ineligLiveIn.insert(V);
            }
        };

        for (llvm::Value *V : model.ineligibleObjs_)
            aggregateIneligVar(V);

        summariesByNode[nodeName] = std::move(agg);
        out.stats.loopsSummarized++;
        if (isStripMined)
            out.stats.stripMinedLoopsSummarized++;
    }

    // Build concrete-to-abstract mapping.
    // Each concrete BB maps to either itself (if not summarized) or its
    // summary node name.
    DenseMap<const BasicBlock *, std::string> concreteToAbstract;
    for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
        concreteToAbstract[BB] = cfg.getBlockInfo(BB).name;
    }
    for (const auto &[nodeName, agg] : summariesByNode) {
        (void)nodeName;
        for (const BasicBlock *BB : agg.loopBlocks) {
            concreteToAbstract[BB] = agg.nodeName;
        }
    }

    // Build ordered list of abstract block names (preserving function order).
    std::vector<std::string> abstractBlocks;
    {
        std::set<std::string> seenNodes;
        for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
            const std::string &node = concreteToAbstract[BB];
            if (seenNodes.insert(node).second) {
                abstractBlocks.push_back(node);
            }
        }
    }

    std::string abstractEntry = concreteToAbstract[cfg.getEntryBlock()];

    std::vector<std::pair<std::string, std::string>> abstractEdges;
    {
        std::set<std::pair<std::string, std::string>> edgeSet;
        for (const auto &[src, dst] : cfg.getEdges()) {
            const std::string &aSrc = concreteToAbstract[src];
            const std::string &aDst = concreteToAbstract[dst];
            bool isUncollapsedConcreteSelfEdge = (src == dst) &&
                                                 (aSrc == cfg.getBlockInfo(src).name) &&
                                                 (aDst == cfg.getBlockInfo(dst).name);
            if (aSrc == aDst && !isUncollapsedConcreteSelfEdge) {
                continue;
            }
            if (edgeSet.insert(std::make_pair(aSrc, aDst)).second) {
                abstractEdges.emplace_back(aSrc, aDst);
            }
        }
    }

    std::vector<std::string> abstractExitBlocks;
    {
        std::set<std::string> exitSeen;
        for (const llvm::BasicBlock *exitBlock : cfg.getExitBlocks()) {
            const std::string &node = concreteToAbstract[exitBlock];
            if (exitSeen.insert(node).second) {
                abstractExitBlocks.push_back(node);
            }
        }
    }

    // Build reverse lookup: node name -> BasicBlock* for non-summarized blocks.
    std::map<std::string, const BasicBlock *> nameToBBConcrete;
    for (const llvm::BasicBlock *BB : cfg.getBlocks()) {
        if (!summarizedConcreteBlocks.count(BB)) {
            nameToBBConcrete[cfg.getBlockInfo(BB).name] = BB;
        }
    }

    // Populate per-abstract-node data.
    std::map<std::string, double> blockEnergyByAbstract;
    std::map<std::string, std::set<llvm::GlobalVariable *>> eligLiveInByAbstract;
    std::map<std::pair<std::string, llvm::GlobalVariable *>, bool> eligDefByAbstract;
    std::map<std::string, std::set<llvm::Value *>> ineligLiveInByAbstract;
    std::map<std::pair<std::string, llvm::Value *>, bool> ineligDefByAbstract;
    std::map<std::pair<std::string, llvm::GlobalVariable *>, double> eNvmByAbstract;
    std::map<std::string, double> fEntryByAbstract;
    std::map<std::string, double> fBoundaryByAbstract;

    for (const std::string &node : abstractBlocks) {
        auto summaryIt = summariesByNode.find(node);
        if (summaryIt != summariesByNode.end()) {
            const LoopAggregate &agg = summaryIt->second;
            blockEnergyByAbstract[node] = agg.pathEnergy;
            fEntryByAbstract[node] = agg.fEntry;

            // Loop entry frequency for boundary costing.
            double fBound = agg.fEntry;
            if (llvm::Loop *L = LI.getLoopFor(agg.headerBB)) {
                if (llvm::BasicBlock *PH = L->getLoopPreheader()) {
                    fBound = energy.getFEntry(PH);
                }
            }
            fBoundaryByAbstract[node] = fBound;
            eligLiveInByAbstract[node] = agg.eligLiveIn;
            ineligLiveInByAbstract[node] = agg.ineligLiveIn;

            // Eligible summary data.
            for (llvm::GlobalVariable *GV : model.vmObjs_) {
                if (agg.eligDefGlobals.count(GV)) {
                    eligDefByAbstract[std::make_pair(node, GV)] = true;
                }
                auto eIt = agg.eNvmByGV.find(GV);
                if (eIt != agg.eNvmByGV.end()) {
                    eNvmByAbstract[std::make_pair(node, GV)] = eIt->second;
                }
            }
            // Ineligible summary data.
            for (llvm::Value *V : model.ineligibleObjs_) {
                if (agg.ineligDefVars.count(V)) {
                    ineligDefByAbstract[std::make_pair(node, V)] = true;
                }
            }
            continue;
        }

        // Concrete (non-summarized) node — look up BB via reverse map.
        auto concreteIt = nameToBBConcrete.find(node);
        if (concreteIt == nameToBBConcrete.end())
            continue;
        const llvm::BasicBlock *concreteBB = concreteIt->second;

        blockEnergyByAbstract[node] = cfg.getBlockInfo(concreteBB).energyCost;
        fEntryByAbstract[node] = energy.getFEntry(concreteBB);
        fBoundaryByAbstract[node] = energy.getFEntry(concreteBB);
        eligLiveInByAbstract[node] = state.getEligLiveIn(concreteBB);
        ineligLiveInByAbstract[node] = state.getIneligLiveIn(concreteBB);

        for (llvm::GlobalVariable *GV : model.vmObjs_) {
            if (state.getEligDefIndicator(concreteBB, GV)) {
                eligDefByAbstract[std::make_pair(node, GV)] = true;
            }
            double nvm = energy.getENvm(concreteBB, GV);
            if (nvm != 0.0) {
                eNvmByAbstract[std::make_pair(node, GV)] = nvm;
            }
        }
        for (llvm::Value *V : model.ineligibleObjs_) {
            if (state.getIneligDefIndicator(concreteBB, V)) {
                ineligDefByAbstract[std::make_pair(node, V)] = true;
            }
        }
    }

    std::map<std::string, NodeId> nodeIdByName;
    NodeId nextNodeId = 0;
    for (const std::string &nodeName : abstractBlocks) {
        NodeId nodeId = nextNodeId++;
        nodeIdByName[nodeName] = nodeId;

        model.blocks_.push_back(nodeId);
        model.nodeNames_[nodeId] = nodeName;

        auto summaryIt = summariesByNode.find(nodeName);
        if (summaryIt != summariesByNode.end()) {
            const LoopAggregate &agg = summaryIt->second;
            auto *header = const_cast<llvm::BasicBlock *>(agg.headerBB);
            // Use the preheader as the representative block.
            llvm::BasicBlock *rep = header;
            if (llvm::Loop *L = LI.getLoopFor(header)) {
                if (llvm::BasicBlock *PH = L->getLoopPreheader())
                    rep = PH;
            }
            model.nodeMap_.setSummaryRepresentative(nodeId, rep);
            // Register all loop-interior blocks → summary NodeId.
            for (const BasicBlock *loopBB : agg.loopBlocks) {
                model.nodeMap_.setSummaryMember(nodeId, const_cast<llvm::BasicBlock *>(loopBB));
            }
        } else {
            auto cIt = nameToBBConcrete.find(nodeName);
            if (cIt != nameToBBConcrete.end()) {
                model.nodeMap_.setConcreteNode(nodeId, const_cast<llvm::BasicBlock *>(cIt->second));
            }
        }
    }

    auto entryIt = nodeIdByName.find(abstractEntry);
    if (entryIt != nodeIdByName.end()) {
        model.entryBlock_ = entryIt->second;
    }

    for (const auto &[srcName, dstName] : abstractEdges) {
        auto srcIt = nodeIdByName.find(srcName);
        auto dstIt = nodeIdByName.find(dstName);
        if (srcIt == nodeIdByName.end() || dstIt == nodeIdByName.end()) {
            continue;
        }
        model.edges_.emplace_back(srcIt->second, dstIt->second);
    }

    for (const std::string &exitName : abstractExitBlocks) {
        auto it = nodeIdByName.find(exitName);
        if (it != nodeIdByName.end()) {
            model.exitBlocks_.push_back(it->second);
        }
    }

    for (const auto &[name, val] : blockEnergyByAbstract) {
        auto it = nodeIdByName.find(name);
        if (it != nodeIdByName.end()) {
            model.blockEnergyCost_[it->second] = val;
        }
    }

    for (const auto &[name, val] : fEntryByAbstract) {
        auto it = nodeIdByName.find(name);
        if (it != nodeIdByName.end()) {
            model.fEntry_[it->second] = val;
        }
    }

    for (const auto &[name, val] : fBoundaryByAbstract) {
        auto it = nodeIdByName.find(name);
        if (it != nodeIdByName.end()) {
            model.fBoundary_[it->second] = val;
        }
    }

    // Eligible live-in by NodeId.
    for (const auto &[name, gvs] : eligLiveInByAbstract) {
        auto it = nodeIdByName.find(name);
        if (it != nodeIdByName.end()) {
            model.eligLiveIn_[it->second] = gvs;
        }
    }

    // Ineligible live-in by NodeId.
    for (const auto &[name, vals] : ineligLiveInByAbstract) {
        auto it = nodeIdByName.find(name);
        if (it != nodeIdByName.end()) {
            model.ineligLiveIn_[it->second] = vals;
        }
    }

    // Eligible def indicators by NodeId.
    for (const auto &[key, val] : eligDefByAbstract) {
        auto it = nodeIdByName.find(key.first);
        if (it != nodeIdByName.end()) {
            model.eligDefIndicator_[std::make_pair(it->second, key.second)] = val;
        }
    }

    // Ineligible def indicators by NodeId.
    for (const auto &[key, val] : ineligDefByAbstract) {
        auto it = nodeIdByName.find(key.first);
        if (it != nodeIdByName.end()) {
            model.ineligDefIndicator_[std::make_pair(it->second, key.second)] = val;
        }
    }

    for (const auto &[key, val] : eNvmByAbstract) {
        auto it = nodeIdByName.find(key.first);
        if (it != nodeIdByName.end()) {
            model.eNvm_[std::make_pair(it->second, key.second)] = val;
        }
    }

    out.stats.abstractNodes = static_cast<unsigned>(model.blocks_.size());
    out.stats.abstractEdges = static_cast<unsigned>(model.edges_.size());

    // Verify that loop collapsing preserved the edge-split invariant:
    // every predecessor of a merge point has exactly one predecessor.
    // With canonical loop form (LoopSimplify guarantees single preheader
    // + single latch), summary nodes should not violate this.
    {
        std::map<NodeId, std::vector<NodeId>> predMap;
        for (NodeId b : model.blocks_)
            predMap[b] = {};
        for (const auto &[src, dst] : model.edges_)
            predMap[dst].push_back(src);
        for (const auto &[block, preds] : predMap) {
            if (preds.size() <= 1)
                continue;
            for (NodeId pred : preds) {
                assert(predMap[pred].size() == 1 &&
                       "AbstractCFG: predecessor of merge-point node has != 1 "
                       "predecessor — EdgeSplit invariant violated by loop "
                       "collapsing");
            }
        }
    }

    return out;
}

} // namespace checkpoint
