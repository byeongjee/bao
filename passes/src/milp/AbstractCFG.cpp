#include "milp/AbstractCFG.h"

#include "common/BlockUtils.h"
#include "common/CFGAnalysis.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/CFG.h"
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

struct PathSummary {
    bool ok = false;
    double energy = 0.0;
    std::set<std::string> blocksOnPath;
    std::string error;
};

struct LoopAggregate {
    std::string nodeName;
    std::string headerName;
    std::set<std::string> loopBlocks;
    std::set<std::string> pathBlocks;
    double pathEnergy = 0.0;
    std::map<llvm::GlobalVariable *, double> eNvmByGV;
    std::set<llvm::GlobalVariable *> liveIn;
    std::set<llvm::GlobalVariable *> defGlobals;
    double fEntry = 1.0;
    double qReboot = 1.0;
};

enum class VisitState {
    Unvisited = 0,
    Visiting,
    Visited,
};

static bool containsInvoke(const Loop *L) {
    for (const BasicBlock *BB : L->blocks()) {
        for (const Instruction &I : *BB) {
            if (isa<InvokeInst>(I)) {
                return true;
            }
        }
    }
    return false;
}

static void collectInnermostFirst(Loop *L, std::vector<Loop *> &out) {
    for (Loop *Sub : L->getSubLoops()) {
        collectInnermostFirst(Sub, out);
    }
    out.push_back(L);
}

static std::vector<Loop *> collectInnermostFirst(LoopInfo &LI) {
    std::vector<Loop *> loops;
    for (Loop *L : LI) {
        collectInnermostFirst(L, loops);
    }
    return loops;
}

static PathSummary computeWorstCasePathSummary(
    Loop *L,
    const std::map<std::string, double> &blockEnergyByName,
    const Function &F) {
    PathSummary result;

    BasicBlock *Header = L->getHeader();
    BasicBlock *Latch = L->getLoopLatch();
    if (!Header || !Latch) {
        result.error = "missing-header-or-latch";
        return result;
    }

    auto getEnergy = [&](const BasicBlock *BB) -> double {
        auto it = blockEnergyByName.find(getBlockName(*BB, F));
        if (it == blockEnergyByName.end()) {
            return 0.0;
        }
        return it->second;
    };

    if (Header == Latch) {
        result.ok = true;
        result.energy = getEnergy(Header);
        result.blocksOnPath.insert(getBlockName(*Header, F));
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

    std::set<std::string> pathBlocks;
    const BasicBlock *cur = Header;
    while (cur) {
        pathBlocks.insert(getBlockName(*cur, F));
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

    if (pathBlocks.empty() ||
        !pathBlocks.count(getBlockName(*Latch, F))) {
        result.error = "path-reconstruction-failed";
        return result;
    }

    result.ok = true;
    result.energy = energy;
    result.blocksOnPath = std::move(pathBlocks);
    return result;
}

static bool overlapsSelected(Loop *L, const std::set<std::string> &selectedBlocks,
                             const Function &F) {
    for (const BasicBlock *BB : L->blocks()) {
        if (selectedBlocks.count(getBlockName(*BB, F))) {
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

double AbstractCFG::getBlockEnergyCost(const std::string &block) const {
    auto it = blockEnergyCost_.find(block);
    if (it != blockEnergyCost_.end()) {
        return it->second;
    }
    return 0.0;
}

const std::set<llvm::GlobalVariable *> &
AbstractCFG::getVMObjLiveIn(const std::string &block) const {
    auto it = vmObjLiveIn_.find(block);
    if (it != vmObjLiveIn_.end()) {
        return it->second;
    }
    static const std::set<llvm::GlobalVariable *> kEmpty;
    return kEmpty;
}

bool AbstractCFG::getDefIndicator(const std::string &block,
                                       llvm::GlobalVariable *gv) const {
    auto it = defIndicator_.find(std::make_pair(block, gv));
    if (it != defIndicator_.end()) {
        return it->second;
    }
    return false;
}

int AbstractCFG::getVMObjSizeBytes(llvm::GlobalVariable *gv) const {
    auto it = vmObjSizeBytes_.find(gv);
    if (it != vmObjSizeBytes_.end()) {
        return it->second;
    }
    return 0;
}

double AbstractCFG::getEBase(const std::string &block) const {
    auto it = blockEnergyCost_.find(block);
    if (it != blockEnergyCost_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getENvm(const std::string &block,
                                 llvm::GlobalVariable *gv) const {
    auto it = eNvm_.find(std::make_pair(block, gv));
    if (it != eNvm_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getESave(llvm::GlobalVariable *gv) const {
    auto it = eSaveByGV_.find(gv);
    if (it != eSaveByGV_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getERestore(llvm::GlobalVariable *gv) const {
    auto it = eRestoreByGV_.find(gv);
    if (it != eRestoreByGV_.end()) {
        return it->second;
    }
    return 0.0;
}

double AbstractCFG::getFEntry(const std::string &block) const {
    auto it = fEntry_.find(block);
    if (it != fEntry_.end()) {
        return it->second;
    }
    return 1.0;
}

double AbstractCFG::getQReboot(const std::string &block) const {
    auto it = qReboot_.find(block);
    if (it != qReboot_.end()) {
        return it->second;
    }
    return params_.qRebootProb;
}

AbstractCFGBuildResult buildAbstractCFG(llvm::Function &F,
                                             llvm::LoopInfo &LI,
                                             const CFGAnalysis &cfg,
                                             const StateAnalysis &state,
                                             const EnergyModel &energy) {
    AbstractCFGBuildResult out;
    out.model = std::make_unique<AbstractCFG>();
    AbstractCFG &model = *out.model;

    model.params_ = energy.getParams();
    model.vmObjs_ = state.getVMObjs();
    for (llvm::GlobalVariable *GV : model.vmObjs_) {
        int elemId = state.getVMObjStateElemId(GV);
        if (elemId >= 0) {
            model.vmObjSizeBytes_[GV] = static_cast<int>(
                state.getStateElement(static_cast<unsigned>(elemId)).sizeBytes);
        }
        model.eSaveByGV_[GV] = energy.getESave(GV);
        model.eRestoreByGV_[GV] = energy.getERestore(GV);
    }

    std::map<std::string, double> blockEnergyByName;
    std::set<std::string> usedNodeNames;
    for (const std::string &block : cfg.getBlocks()) {
        blockEnergyByName[block] = cfg.getBlockInfo(block).energyCost;
        usedNodeNames.insert(block);
    }

    std::vector<Loop *> loops = collectInnermostFirst(LI);
    std::set<std::string> summarizedConcreteBlocks;
    std::map<std::string, LoopAggregate> summariesByNode;

    const double budget = model.params_.capacity - model.params_.E_pro - model.params_.E_epi;

    for (Loop *L : loops) {
        out.stats.loopsSeen++;

        if (budget <= 0.0) {
            out.stats.skippedReasons["nonpositive-energy-budget"]++;
            continue;
        }
        if (!L->getHeader() || !L->getLoopLatch()) {
            out.stats.skippedReasons["missing-header-or-latch"]++;
            continue;
        }
        if (containsInvoke(L)) {
            out.stats.skippedReasons["contains-invoke"]++;
            continue;
        }
        if (overlapsSelected(L, summarizedConcreteBlocks, F)) {
            out.stats.skippedReasons["overlaps-summarized-loop"]++;
            continue;
        }

        PathSummary path = computeWorstCasePathSummary(L, blockEnergyByName, F);
        if (!path.ok) {
            out.stats.skippedReasons[path.error]++;
            continue;
        }
        if (path.energy <= 0.0) {
            out.stats.skippedReasons["nonpositive-loop-energy"]++;
            continue;
        }

        out.stats.loopsEligible++;
        if (!(path.energy < budget)) {
            out.stats.skippedReasons["loop-unit-exceeds-budget"]++;
            continue;
        }

        std::string headerName = getBlockName(*L->getHeader(), F);
        std::string nodeName = makeUniqueSummaryNodeName(headerName, usedNodeNames);
        usedNodeNames.insert(nodeName);

        LoopAggregate agg;
        agg.nodeName = nodeName;
        agg.headerName = headerName;
        agg.pathBlocks = path.blocksOnPath;
        agg.pathEnergy = path.energy;
        agg.fEntry = energy.getFEntry(headerName);
        agg.qReboot = energy.getQReboot(headerName);

        for (const BasicBlock *BB : L->blocks()) {
            std::string blockName = getBlockName(*BB, F);
            agg.loopBlocks.insert(blockName);
            summarizedConcreteBlocks.insert(blockName);
        }

        for (llvm::GlobalVariable *GV : model.vmObjs_) {
            double nvmSum = 0.0;
            bool hasDef = false;
            bool hasLiveIn = false;

            for (const std::string &blockName : agg.pathBlocks) {
                nvmSum += energy.getENvm(blockName, GV);
                hasDef |= state.getDefIndicator(blockName, GV);
                hasLiveIn |= state.getVMObjLiveIn(blockName).count(GV) > 0;
            }

            if (nvmSum != 0.0) {
                agg.eNvmByGV[GV] = nvmSum;
            }
            if (hasDef) {
                agg.defGlobals.insert(GV);
            }
            if (hasLiveIn) {
                agg.liveIn.insert(GV);
            }
        }

        summariesByNode[nodeName] = agg;
        out.abstractToConcreteRepresentative[nodeName] = headerName;
        out.stats.loopsSummarized++;
    }

    std::map<std::string, std::string> concreteToAbstract;
    for (const std::string &block : cfg.getBlocks()) {
        concreteToAbstract[block] = block;
    }
    for (const auto &[nodeName, agg] : summariesByNode) {
        (void)nodeName;
        for (const std::string &block : agg.loopBlocks) {
            concreteToAbstract[block] = agg.nodeName;
        }
    }

    std::set<std::string> seenNodes;
    for (const std::string &block : cfg.getBlocks()) {
        const std::string &node = concreteToAbstract[block];
        if (seenNodes.insert(node).second) {
            model.blocks_.push_back(node);
        }
    }

    model.entryBlock_ = concreteToAbstract[cfg.getEntryBlock()];

    std::set<std::pair<std::string, std::string>> edgeSet;
    for (const auto &[src, dst] : cfg.getEdges()) {
        std::string aSrc = concreteToAbstract[src];
        std::string aDst = concreteToAbstract[dst];
        // Preserve true concrete self-edges when no abstraction occurred.
        // Drop only edges that collapsed due to loop summarization.
        bool isUncollapsedConcreteSelfEdge =
            (src == dst) && (aSrc == src) && (aDst == dst);
        if (aSrc == aDst && !isUncollapsedConcreteSelfEdge) {
            continue;
        }
        if (edgeSet.insert(std::make_pair(aSrc, aDst)).second) {
            model.edges_.emplace_back(aSrc, aDst);
        }
    }

    std::set<std::string> exitSeen;
    for (const std::string &exitBlock : cfg.getExitBlocks()) {
        std::string node = concreteToAbstract[exitBlock];
        if (exitSeen.insert(node).second) {
            model.exitBlocks_.push_back(node);
        }
    }

    for (const std::string &node : model.blocks_) {
        auto summaryIt = summariesByNode.find(node);
        if (summaryIt != summariesByNode.end()) {
            const LoopAggregate &agg = summaryIt->second;
            model.blockEnergyCost_[node] = agg.pathEnergy;
            model.fEntry_[node] = agg.fEntry;
            model.qReboot_[node] = agg.qReboot;
            model.vmObjLiveIn_[node] = agg.liveIn;
            for (llvm::GlobalVariable *GV : model.vmObjs_) {
                if (agg.defGlobals.count(GV)) {
                    model.defIndicator_[std::make_pair(node, GV)] = true;
                }
                auto eIt = agg.eNvmByGV.find(GV);
                if (eIt != agg.eNvmByGV.end()) {
                    model.eNvm_[std::make_pair(node, GV)] = eIt->second;
                }
            }
            continue;
        }

        model.blockEnergyCost_[node] = cfg.getBlockInfo(node).energyCost;
        model.fEntry_[node] = energy.getFEntry(node);
        model.qReboot_[node] = energy.getQReboot(node);
        model.vmObjLiveIn_[node] = state.getVMObjLiveIn(node);
        for (llvm::GlobalVariable *GV : model.vmObjs_) {
            if (state.getDefIndicator(node, GV)) {
                model.defIndicator_[std::make_pair(node, GV)] = true;
            }
            double nvm = energy.getENvm(node, GV);
            if (nvm != 0.0) {
                model.eNvm_[std::make_pair(node, GV)] = nvm;
            }
        }
    }

    for (const std::string &node : model.blocks_) {
        if (!out.abstractToConcreteRepresentative.count(node)) {
            out.abstractToConcreteRepresentative[node] = node;
        }
    }

    out.stats.abstractNodes = static_cast<unsigned>(model.blocks_.size());
    out.stats.abstractEdges = static_cast<unsigned>(model.edges_.size());
    return out;
}

} // namespace checkpoint
