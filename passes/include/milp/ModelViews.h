#pragma once

#include "common/NodeMap.h"
#include "milp/EnergyModel.h"

#include "llvm/IR/GlobalVariable.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace checkpoint {

/// Abstract CFG view consumed by the MILP optimizer.
class ICFGView {
public:
    virtual ~ICFGView() = default;

    virtual const std::vector<NodeId> &getBlocks() const = 0;
    virtual const std::vector<std::pair<NodeId, NodeId>> &getEdges() const = 0;
    virtual NodeId getEntryBlock() const = 0;
    virtual const std::vector<NodeId> &getExitBlocks() const = 0;
    virtual double getBlockEnergyCost(NodeId block) const = 0;

    // For diagnostics/human-readable output only.
    virtual const std::string &getNodeName(NodeId node) const = 0;
    virtual const NodeMap &getNodeMap() const = 0;
};

/// Abstract state-analysis view consumed by the MILP optimizer.
class IStateView {
public:
    virtual ~IStateView() = default;

    virtual const std::vector<llvm::GlobalVariable *> &getVMObjs() const = 0;
    virtual const std::set<llvm::GlobalVariable *> &
    getVMObjLiveIn(NodeId block) const = 0;
    virtual bool getDefIndicator(NodeId block, llvm::GlobalVariable *gv) const = 0;
    virtual int getVMObjSizeBytes(llvm::GlobalVariable *gv) const = 0;
};

/// Abstract energy-model view consumed by the MILP optimizer.
class IEnergyView {
public:
    virtual ~IEnergyView() = default;

    virtual const MILPEnergyParams &getParams() const = 0;
    virtual double getEBase(NodeId block) const = 0;
    virtual double getENvm(NodeId block, llvm::GlobalVariable *gv) const = 0;
    virtual double getESave(llvm::GlobalVariable *gv) const = 0;
    virtual double getERestore(llvm::GlobalVariable *gv) const = 0;
    virtual double getFEntry(NodeId block) const = 0;
    virtual double getQReboot() const = 0;
};

} // namespace checkpoint
