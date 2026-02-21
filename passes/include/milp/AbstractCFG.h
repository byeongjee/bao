#pragma once

#include "common/NodeMap.h"
#include "milp/ModelViews.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Value.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace checkpoint {

class CFGAnalysis;
class StateAnalysis;
class EnergyModel;
struct AbstractCFGBuildResult;

struct AbstractCFGStats {
    unsigned loopsSeen = 0;
    unsigned loopsEligible = 0;
    unsigned loopsSummarized = 0;
    unsigned stripMinedLoopsSeen = 0;
    unsigned stripMinedLoopsSummarized = 0;
    unsigned stripMinedLoopsSkipped = 0;
    std::map<std::string, unsigned> skippedReasons;
    unsigned abstractNodes = 0;
    unsigned abstractEdges = 0;
};

/// MILP view backed by loop-collapsed abstract CFG/state/energy data.
class AbstractCFG final : public ICFGView,
                          public IStateView,
                          public IEnergyView {
public:
    using BlockGVKey = std::pair<NodeId, llvm::GlobalVariable *>;
    using BlockVarKey = std::pair<NodeId, llvm::Value *>;

    AbstractCFG() = default;

    // ICFGView
    const std::vector<NodeId> &getBlocks() const override { return blocks_; }
    const std::vector<std::pair<NodeId, NodeId>> &getEdges() const override {
        return edges_;
    }
    NodeId getEntryBlock() const override { return entryBlock_; }
    const std::vector<NodeId> &getExitBlocks() const override {
        return exitBlocks_;
    }
    double getBlockEnergyCost(NodeId block) const override;
    const std::string &getNodeName(NodeId node) const override;
    const NodeMap &getNodeMap() const override { return nodeMap_; }

    // IStateView — eligible
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const override {
        return vmObjs_;
    }
    const std::set<llvm::GlobalVariable *> &
    getEligLiveIn(NodeId block) const override;
    bool getEligDefIndicator(NodeId block,
                             llvm::GlobalVariable *gv) const override;

    // IStateView — ineligible
    const std::vector<llvm::Value *> &getIneligibleObjs() const override;
    bool isIneligible(llvm::Value *v) const override;
    const std::set<llvm::Value *> &
    getIneligLiveIn(NodeId block) const override;
    bool getIneligDefIndicator(NodeId block,
                               llvm::Value *v) const override;

    // IStateView — shared
    int getVarSizeBytes(llvm::Value *v) const override;

    // IEnergyView
    const MILPEnergyParams &getParams() const override { return params_; }
    double getEBase(NodeId block) const override;
    double getENvm(NodeId block, llvm::GlobalVariable *gv) const override;
    double getESave(llvm::Value *v) const override;
    double getERestore(llvm::Value *v) const override;
    double getFEntry(NodeId block) const override;
    double getFBoundary(NodeId block) const override;
    double getQReboot() const override;

private:
    friend struct AbstractCFGBuildResult;
    friend AbstractCFGBuildResult buildAbstractCFG(llvm::Function &F,
                                                   llvm::LoopInfo &LI,
                                                   llvm::ScalarEvolution &SE,
                                                   const CFGAnalysis &cfg,
                                                   const StateAnalysis &state,
                                                   const EnergyModel &energy);

    NodeMap nodeMap_;

    std::vector<NodeId> blocks_;
    std::vector<std::pair<NodeId, NodeId>> edges_;
    NodeId entryBlock_ = kInvalidNodeId;
    std::vector<NodeId> exitBlocks_;

    std::map<NodeId, std::string> nodeNames_;
    std::map<NodeId, double> blockEnergyCost_;

    // Eligible (candidate globals)
    std::vector<llvm::GlobalVariable *> vmObjs_;
    std::map<NodeId, std::set<llvm::GlobalVariable *>> eligLiveIn_;
    std::map<BlockGVKey, bool> eligDefIndicator_;

    // Ineligible (non-candidate globals, allocas, SSA values)
    std::vector<llvm::Value *> ineligibleObjs_;
    std::set<llvm::Value *> ineligibleObjSet_;
    std::map<NodeId, std::set<llvm::Value *>> ineligLiveIn_;
    std::map<BlockVarKey, bool> ineligDefIndicator_;

    // Shared size map
    std::map<llvm::Value *, int> varSizeBytes_;

    MILPEnergyParams params_{};
    std::map<BlockGVKey, double> eNvm_;
    std::map<llvm::Value *, double> eSaveByVar_;
    std::map<llvm::Value *, double> eRestoreByVar_;
    std::map<NodeId, double> fEntry_;
    std::map<NodeId, double> fBoundary_;
};

struct AbstractCFGBuildResult {
    std::unique_ptr<AbstractCFG> model;
    AbstractCFGStats stats;
};

/// Build loop-collapsed abstract model used by MILP.
AbstractCFGBuildResult buildAbstractCFG(llvm::Function &F,
                                        llvm::LoopInfo &LI,
                                        llvm::ScalarEvolution &SE,
                                        const CFGAnalysis &cfg,
                                        const StateAnalysis &state,
                                        const EnergyModel &energy);

} // namespace checkpoint
