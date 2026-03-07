#pragma once

#include "milp/ModelViews.h"

#include "gurobi_c++.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Input data for the MILP optimizer (aggregates all analysis results).
struct MILPInput {
    const ICFGView &cfg;
    const IStateView &state;
    const IEnergyView &energy;
};

/// Solver optimality status.
enum class SolverStatus {
    Optimal,  ///< Proven optimal solution.
    Feasible, ///< Feasible but not proven optimal (e.g., time limit).
};

/// Solution from the MILP optimizer.
struct MILPSolution {
    /// Nodes where is_region_start = 1.
    std::set<NodeId> regionStarts;

    /// place_in_vm[b,v] values (eligible only).
    std::map<std::pair<NodeId, llvm::GlobalVariable *>, bool> placeInVm;

    /// need_restore[b,v] values (eligible only, for v in EligLiveIn(b)).
    std::map<std::pair<NodeId, llvm::GlobalVariable *>, bool> needRestore;

    /// commit[b,v] values (for b != b0 and v in LiveIn(b)).
    /// Uses Value* to handle both eligible globals and ineligible objects.
    std::map<std::pair<NodeId, llvm::Value *>, bool> commit;

    /// energy_accumulated[b] values.
    std::map<NodeId, double> energyAccumulated;

    /// Objective function value.
    double objectiveValue = 0.0;

    /// Solver optimality status.
    SolverStatus solverStatus = SolverStatus::Optimal;

    /// MIP optimality gap (0.0 for optimal, >0 for feasible).
    double mipGap = 0.0;

    /// Get all Value*s with commit=true at a given node.
    std::set<llvm::Value *> getCommitVarsAt(NodeId node) const {
        std::set<llvm::Value *> result;
        for (const auto &[key, enabled] : commit)
            if (enabled && key.first == node)
                result.insert(key.second);
        return result;
    }

    /// Get all GlobalVariable*s with needRestore=true at a given node.
    std::set<llvm::GlobalVariable *> getRestoreGVsAt(NodeId node) const {
        std::set<llvm::GlobalVariable *> result;
        for (const auto &[key, enabled] : needRestore)
            if (enabled && key.first == node)
                result.insert(key.second);
        return result;
    }

    /// Count entries with value=true in a pair-keyed map.
    template <typename K>
    static unsigned countEnabled(const std::map<std::pair<NodeId, K>, bool> &m) {
        unsigned n = 0;
        for (const auto &[key, enabled] : m)
            if (enabled)
                ++n;
        return n;
    }
};

/// MILP optimizer for checkpoint placement using Gurobi.
class CheckpointOptimizer {
  public:
    CheckpointOptimizer(const MILPInput &input);

    bool solve();

    void setAcceptFeasible(bool accept) { acceptFeasible_ = accept; }
    void setTimeLimit(double seconds) { timeLimit_ = seconds; }

    const MILPSolution &getSolution() const { return solution_; }

    std::set<NodeId> getCheckpoints() const { return solution_.regionStarts; }

    double getObjectiveValue() const { return solution_.objectiveValue; }

    int getNumVars() const;
    int getNumConstrs() const;

    /// Check feasibility - returns nodes whose base energy exceeds capacity.
    std::vector<NodeId> getInfeasibleBlocks() const;

  private:
    const ICFGView &cfg_;
    const IStateView &state_;
    const IEnergyView &energy_;
    const MILPEnergyParams &params_;

    GRBEnv env_;
    GRBModel model_;
    MILPSolution solution_;
    bool solved_ = false;
    bool acceptFeasible_ = false;
    double timeLimit_ = 600.0;

    using BlockGVKey = std::pair<NodeId, llvm::GlobalVariable *>;
    using BlockVarKey = std::pair<NodeId, llvm::Value *>;

    // MILP variables
    std::map<NodeId, GRBVar> isRegionStart_;     // x[b]
    std::map<BlockGVKey, GRBVar> placeInVm_;     // p[b,v] — eligible only
    std::map<BlockGVKey, GRBVar> needRestore_;   // y[b,v] — eligible only
    std::map<BlockVarKey, GRBVar> pending_;      // pending[b,v] — all tracked
    std::map<BlockGVKey, GRBVar> vmPending_;     // vm_pending[b,v] — eligible only
    std::map<BlockVarKey, GRBVar> commit_;       // commit[b,v] — all tracked
    std::map<NodeId, GRBVar> energyAccumulated_; // eaccum[b]

    std::map<NodeId, std::vector<NodeId>> predecessors_;

    void buildModel();
    void addVariables();
    void addObjective();
    void addConstraints();
    void extractSolution();

    // Constraint helpers
    void constrainEntryAsRegionStart();
    void constrainVMCapacity();
    void constrainNeedRestoreLinearization();
    void constrainPlacementPropagation();
    void constrainPendingStatePropagation();
    void constrainCommitAtRegionBoundary();
    void constrainEnergyInitAtRegionStart();
    void constrainEnergyPropagation();
    void constrainEnergyWithinCapacity();

    // Expression builders (linear in decision variables)
    GRBLinExpr buildEBlk(NodeId block);
    GRBLinExpr buildEStart(NodeId block);
    GRBLinExpr buildEEnd(NodeId block);
};

} // namespace checkpoint
