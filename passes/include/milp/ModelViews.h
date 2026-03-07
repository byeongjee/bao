#pragma once

#include "common/NodeMap.h"
#include "milp/EnergyModel.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Value.h"

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
///
/// Eligible globals (V_elig) have placeInVm/needRestore decision variables.
/// Ineligible objects (V_inelig) — non-candidate globals, allocas, cross-block
/// SSA values — always reside in VM and only get pending/commit variables.
class IStateView {
  public:
    virtual ~IStateView() = default;

    // -- Eligible (candidate globals) --
    virtual const std::vector<llvm::GlobalVariable *> &getVMObjs() const = 0;
    virtual const std::set<llvm::GlobalVariable *> &getEligLiveIn(NodeId block) const = 0;
    virtual bool getEligDefIndicator(NodeId block, llvm::GlobalVariable *gv) const = 0;

    // -- Ineligible (non-candidate globals, allocas, SSA registers) --
    virtual const std::vector<llvm::Value *> &getIneligibleObjs() const = 0;
    virtual bool isIneligible(llvm::Value *v) const = 0;
    virtual const std::set<llvm::Value *> &getIneligLiveIn(NodeId block) const = 0;
    virtual bool getIneligDefIndicator(NodeId block, llvm::Value *v) const = 0;

    // -- Shared --
    virtual int getVarSizeBytes(llvm::Value *v) const = 0;
};

/// Abstract energy-model view consumed by the MILP optimizer.
class IEnergyView {
  public:
    virtual ~IEnergyView() = default;

    virtual const MILPEnergyParams &getParams() const = 0;
    virtual double getEBase(NodeId block) const = 0;
    /// NVM access penalty — only meaningful for eligible globals.
    virtual double getENvm(NodeId block, llvm::GlobalVariable *gv) const = 0;
    virtual double getESave(llvm::Value *v) const = 0;
    virtual double getERestore(llvm::Value *v) const = 0;
    virtual double getFEntry(NodeId block) const = 0;
    /// Frequency for boundary (region start/end) cost weighting.
    /// For concrete nodes, equals getFEntry. For summary nodes, equals the
    /// loop entry frequency (preheader), not the per-iteration header frequency.
    virtual double getFBoundary(NodeId block) const = 0;
    virtual double getQReboot() const = 0;
};

} // namespace checkpoint
