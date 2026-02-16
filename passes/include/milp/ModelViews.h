#pragma once

#include "common/CFGAnalysis.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "llvm/IR/GlobalVariable.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace checkpoint {

/// Abstract CFG view consumed by the MILP optimizer.
class ICFGView {
public:
    virtual ~ICFGView() = default;

    virtual const std::vector<std::string> &getBlocks() const = 0;
    virtual const std::vector<std::pair<std::string, std::string>> &getEdges()
        const = 0;
    virtual const std::string &getEntryBlock() const = 0;
    virtual const std::vector<std::string> &getExitBlocks() const = 0;
    virtual double getBlockEnergyCost(const std::string &block) const = 0;
};

/// Abstract state-analysis view consumed by the MILP optimizer.
class IStateView {
public:
    virtual ~IStateView() = default;

    virtual const std::vector<llvm::GlobalVariable *> &getVMObjs() const = 0;
    virtual const std::set<llvm::GlobalVariable *> &
    getVMObjLiveIn(const std::string &block) const = 0;
    virtual bool getDefIndicator(const std::string &block,
                                 llvm::GlobalVariable *gv) const = 0;
    virtual int getVMObjSizeBytes(llvm::GlobalVariable *gv) const = 0;
};

/// Abstract energy-model view consumed by the MILP optimizer.
class IEnergyView {
public:
    virtual ~IEnergyView() = default;

    virtual const MILPEnergyParams &getParams() const = 0;
    virtual double getEBase(const std::string &block) const = 0;
    virtual double getENvm(const std::string &block,
                           llvm::GlobalVariable *gv) const = 0;
    virtual double getESave(llvm::GlobalVariable *gv) const = 0;
    virtual double getERestore(llvm::GlobalVariable *gv) const = 0;
    virtual double getFEntry(const std::string &block) const = 0;
    virtual double getQReboot(const std::string &block) const = 0;
};

/// Adapts concrete CFG/State/Energy analyses to MILP abstract views.
class ConcreteMILPViewAdapter final : public ICFGView,
                                      public IStateView,
                                      public IEnergyView {
public:
    ConcreteMILPViewAdapter(const CFGAnalysis &cfg,
                            const StateAnalysis &state,
                            const EnergyModel &energy)
        : cfg_(cfg), state_(state), energy_(energy) {}

    // ICFGView
    const std::vector<std::string> &getBlocks() const override {
        return cfg_.getBlocks();
    }
    const std::vector<std::pair<std::string, std::string>> &getEdges()
        const override {
        return cfg_.getEdges();
    }
    const std::string &getEntryBlock() const override {
        return cfg_.getEntryBlock();
    }
    const std::vector<std::string> &getExitBlocks() const override {
        return cfg_.getExitBlocks();
    }
    double getBlockEnergyCost(const std::string &block) const override {
        return cfg_.getBlockInfo(block).energyCost;
    }

    // IStateView
    const std::vector<llvm::GlobalVariable *> &getVMObjs() const override {
        return state_.getVMObjs();
    }
    const std::set<llvm::GlobalVariable *> &
    getVMObjLiveIn(const std::string &block) const override {
        return state_.getVMObjLiveIn(block);
    }
    bool getDefIndicator(const std::string &block,
                         llvm::GlobalVariable *gv) const override {
        return state_.getDefIndicator(block, gv);
    }
    int getVMObjSizeBytes(llvm::GlobalVariable *gv) const override {
        int elemId = state_.getVMObjStateElemId(gv);
        if (elemId < 0) {
            return 0;
        }
        return static_cast<int>(
            state_.getStateElement(static_cast<unsigned>(elemId)).sizeBytes);
    }

    // IEnergyView
    const MILPEnergyParams &getParams() const override {
        return energy_.getParams();
    }
    double getEBase(const std::string &block) const override {
        return energy_.getEBase(block);
    }
    double getENvm(const std::string &block,
                   llvm::GlobalVariable *gv) const override {
        return energy_.getENvm(block, gv);
    }
    double getESave(llvm::GlobalVariable *gv) const override {
        return energy_.getESave(gv);
    }
    double getERestore(llvm::GlobalVariable *gv) const override {
        return energy_.getERestore(gv);
    }
    double getFEntry(const std::string &block) const override {
        return energy_.getFEntry(block);
    }
    double getQReboot(const std::string &block) const override {
        return energy_.getQReboot(block);
    }

private:
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const EnergyModel &energy_;
};

} // namespace checkpoint
