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
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
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
    const BasicBlock *entryBB = nullptr;
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

        checkpoint::SummaryBudget summaryBudget =
            checkpoint::computeSummaryBudget(L, blockEnergyByBB, LI, SE, state, model.params_);
        if (!summaryBudget.ok) {
            if (summaryBudget.error == "nonpositive-loop-energy")
                skipLoop(summaryBudget.error,
                         "path-energy=" + std::to_string(summaryBudget.worstCasePath.energy));
            else
                skipLoop(summaryBudget.error, "path-energy-unavailable");
            continue;
        }

        out.stats.loopsEligible++;

        // Compute loop trip count for total energy check.
        unsigned scevTC = SE.getSmallConstantTripCount(L);
        auto markerTC = getMarkerTripCount(L);
        uint64_t loopTC;
        if (scevTC > 0 && markerTC)
            loopTC = std::min<uint64_t>(scevTC, *markerTC);
        else if (scevTC > 0)
            loopTC = scevTC;
        else if (markerTC)
            loopTC = *markerTC;
        else {
            skipLoop("unknown-loop-trip-count",
                     "scev-trip-count=" + std::to_string(scevTC) + ", marker-trip-count=none");
            continue;
        }

        // Preheader belongs to the summary; see SummaryBudget.
        BasicBlock *preheaderBB = L->getLoopPreheader();

        double totalBaseEnergy = summaryBudget.worstCasePath.energy * static_cast<double>(loopTC) +
                                 summaryBudget.preheaderEnergy;
        double totalNvmPenalty = summaryBudget.perIterNvmPenalty * static_cast<double>(loopTC) +
                                 summaryBudget.preheaderNvmPenalty;
        double totalEnergyWithNvm = totalBaseEnergy + totalNvmPenalty;
        // The SummaryBudget predicate; maxKMatchingSummarizer inverts it.
        if (summaryBudget.budgetAfterBoundary <= 0.0 ||
            !(totalEnergyWithNvm < summaryBudget.budgetAfterBoundary)) {
            skipLoop(
                "loop-total-exceeds-budget",
                "path-energy=" + std::to_string(summaryBudget.worstCasePath.energy) +
                    ", per-iter-path-energy=" + std::to_string(summaryBudget.worstCasePath.energy) +
                    ", per-iter-nvm-penalty=" + std::to_string(summaryBudget.perIterNvmPenalty) +
                    ", trip-count=" + std::to_string(loopTC) +
                    ", total-base-energy=" + std::to_string(totalBaseEnergy) +
                    ", total-nvm-penalty=" + std::to_string(totalNvmPenalty) +
                    ", total-energy-with-nvm=" + std::to_string(totalEnergyWithNvm) +
                    ", nvm-access-margin=" + std::to_string(summaryBudget.perIterNvmPenalty) +
                    ", restore-livein-margin=" + std::to_string(summaryBudget.restoreLiveInMargin) +
                    ", commit-def-margin=" + std::to_string(summaryBudget.commitDefMargin) +
                    ", boundary-state-margin=" + std::to_string(summaryBudget.boundaryStateMargin) +
                    ", budget-after-boundary=" + std::to_string(summaryBudget.budgetAfterBoundary));
            continue;
        }

        BasicBlock *headerBB = loopHeader;
        std::string headerName = loopHeaderName;
        std::string nodeName = makeUniqueSummaryNodeName(headerName, usedNodeNames);
        usedNodeNames.insert(nodeName);

        LoopAggregate agg;
        agg.nodeName = nodeName;
        agg.headerBB = headerBB;
        agg.entryBB = preheaderBB ? preheaderBB : headerBB;
        agg.pathBlocks = std::move(summaryBudget.worstCasePath.blocksOnPath);
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
        if (preheaderBB) {
            // Outermost-first summarization: if any enclosing loop were
            // summarized, this loop would have been skipped, so its
            // preheader cannot belong to another summary.
            assert(!summarizedConcreteBlocks.count(preheaderBB) &&
                   "preheader of a summarized loop already belongs to another summary");
            agg.loopBlocks.insert(preheaderBB);
            summarizedConcreteBlocks.insert(preheaderBB);
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

            double totalNvm = nvmSum * static_cast<double>(loopTC);
            if (preheaderBB)
                totalNvm += energy.getENvm(preheaderBB, GV);
            if (totalNvm != 0.0) {
                agg.eNvmByGV[GV] = totalNvm;
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
            // Invariant: summaries never overlap — outermost-first
            // summarization skips any loop that touches an existing summary.
            assert(concreteToAbstract[BB] == cfg.getBlockInfo(BB).name &&
                   "block belongs to more than one summary");
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
            // Invariant: a summary's boundary is emitted at the top of its
            // entry block, so all control flow into the summary must enter
            // there.
            if (auto sIt = summariesByNode.find(aDst); sIt != summariesByNode.end()) {
                assert(dst == sIt->second.entryBB &&
                       "edge into a summary must enter at its entry block");
                (void)sIt;
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

            // Boundary costing uses the loop entry frequency, which is
            // exactly what agg.fEntry already holds (preheader frequency
            // when a preheader exists, header frequency otherwise).
            fBoundaryByAbstract[node] = agg.fEntry;
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
            // The representative block is where the instrumenter emits the
            // summary's boundary (at its top), so it must be the summary's
            // entry block — and in particular a member of the summary.
            auto *rep = const_cast<llvm::BasicBlock *>(agg.entryBB);
            assert(agg.loopBlocks.count(rep) &&
                   "summary representative must be a member of the summary");
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

    return out;
}

} // namespace checkpoint
