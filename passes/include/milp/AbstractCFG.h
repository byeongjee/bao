#pragma once

#include "common/NodeMap.h"
#include "milp/ModelViews.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"

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

    // IStateView
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const override {
        return vmObjs_;
    }
    const std::set<llvm::GlobalVariable *> &
    getVMObjLiveIn(NodeId block) const override;
    bool getDefIndicator(NodeId block,
                         llvm::GlobalVariable *gv) const override;
    int getVMObjSizeBytes(llvm::GlobalVariable *gv) const override;

    // IEnergyView
    const MILPEnergyParams &getParams() const override { return params_; }
    double getEBase(NodeId block) const override;
    double getENvm(NodeId block, llvm::GlobalVariable *gv) const override;
    double getESave(llvm::GlobalVariable *gv) const override;
    double getERestore(llvm::GlobalVariable *gv) const override;
    double getFEntry(NodeId block) const override;
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

    std::vector<llvm::GlobalVariable *> vmObjs_;
    std::map<NodeId, std::set<llvm::GlobalVariable *>> vmObjLiveIn_;
    std::map<BlockGVKey, bool> defIndicator_;
    std::map<llvm::GlobalVariable *, int> vmObjSizeBytes_;

    MILPEnergyParams params_{};
    std::map<BlockGVKey, double> eNvm_;
    std::map<llvm::GlobalVariable *, double> eSaveByGV_;
    std::map<llvm::GlobalVariable *, double> eRestoreByGV_;
    std::map<NodeId, double> fEntry_;
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
