#pragma once

#include "milp/ModelViews.h"

#include "llvm/Analysis/LoopInfo.h"
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
    using BlockGVKey = std::pair<std::string, llvm::GlobalVariable *>;

    AbstractCFG() = default;

    // ICFGView
    const std::vector<std::string> &getBlocks() const override { return blocks_; }
    const std::vector<std::pair<std::string, std::string>> &getEdges()
        const override {
        return edges_;
    }
    const std::string &getEntryBlock() const override { return entryBlock_; }
    const std::vector<std::string> &getExitBlocks() const override {
        return exitBlocks_;
    }
    double getBlockEnergyCost(const std::string &block) const override;

    // IStateView
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const override {
        return vmObjs_;
    }
    const std::set<llvm::GlobalVariable *> &
    getVMObjLiveIn(const std::string &block) const override;
    bool getDefIndicator(const std::string &block,
                         llvm::GlobalVariable *gv) const override;
    int getVMObjSizeBytes(llvm::GlobalVariable *gv) const override;

    // IEnergyView
    const MILPEnergyParams &getParams() const override { return params_; }
    double getEBase(const std::string &block) const override;
    double getENvm(const std::string &block,
                   llvm::GlobalVariable *gv) const override;
    double getESave(llvm::GlobalVariable *gv) const override;
    double getERestore(llvm::GlobalVariable *gv) const override;
    double getFEntry(const std::string &block) const override;
    double getQReboot(const std::string &block) const override;

private:
    friend struct AbstractCFGBuildResult;
    friend AbstractCFGBuildResult buildAbstractCFG(llvm::Function &F,
                                                        llvm::LoopInfo &LI,
                                                        const CFGAnalysis &cfg,
                                                        const StateAnalysis &state,
                                                        const EnergyModel &energy);

    std::vector<std::string> blocks_;
    std::vector<std::pair<std::string, std::string>> edges_;
    std::string entryBlock_;
    std::vector<std::string> exitBlocks_;
    std::map<std::string, double> blockEnergyCost_;

    std::vector<llvm::GlobalVariable *> vmObjs_;
    std::map<std::string, std::set<llvm::GlobalVariable *>> vmObjLiveIn_;
    std::map<BlockGVKey, bool> defIndicator_;
    std::map<llvm::GlobalVariable *, int> vmObjSizeBytes_;

    MILPEnergyParams params_{};
    std::map<BlockGVKey, double> eNvm_;
    std::map<llvm::GlobalVariable *, double> eSaveByGV_;
    std::map<llvm::GlobalVariable *, double> eRestoreByGV_;
    std::map<std::string, double> fEntry_;
    std::map<std::string, double> qReboot_;
};

struct AbstractCFGBuildResult {
    std::unique_ptr<AbstractCFG> model;
    std::map<std::string, std::string> abstractToConcreteRepresentative;
    AbstractCFGStats stats;
};

/// Build loop-collapsed abstract model used by MILP.
///
/// Summarization rule:
///   summarize loop L if E_loop_unit_wc(L) < (capacity - E_pro - E_epi)
AbstractCFGBuildResult buildAbstractCFG(llvm::Function &F,
                                             llvm::LoopInfo &LI,
                                             const CFGAnalysis &cfg,
                                             const StateAnalysis &state,
                                             const EnergyModel &energy);

} // namespace checkpoint
