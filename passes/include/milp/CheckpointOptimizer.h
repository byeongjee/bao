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
};

/// MILP optimizer for checkpoint placement using Gurobi.
class CheckpointOptimizer {
public:
    CheckpointOptimizer(const MILPInput &input);

    bool solve();

    void setAcceptFeasible(bool accept) { acceptFeasible_ = accept; }

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

    using BlockGVKey = std::pair<NodeId, llvm::GlobalVariable *>;
    using BlockVarKey = std::pair<NodeId, llvm::Value *>;

    // MILP variables
    std::map<NodeId, GRBVar> isRegionStart_;         // x[b]
    std::map<BlockGVKey, GRBVar> placeInVm_;         // p[b,v] — eligible only
    std::map<BlockGVKey, GRBVar> needRestore_;       // y[b,v] — eligible only
    std::map<BlockVarKey, GRBVar> pending_;           // pending[b,v] — all tracked
    std::map<BlockGVKey, GRBVar> vmPending_;          // vm_pending[b,v] — eligible only
    std::map<BlockVarKey, GRBVar> commit_;            // commit[b,v] — all tracked
    std::map<NodeId, GRBVar> energyAccumulated_;     // eaccum[b]

    void buildModel();
    void addVariables();
    void addObjective();
    void addConstraints();
    void extractSolution();

    // Constraint helpers
    void addC1_EntryRegionStart();
    void addC3_VMCapacity();
    void addC4_NeedVolLinearization();
    void addC5_PlacementPropagation();
    void addC6_PendingPropagation();
    void addC7_CommitModel();
    void addC8_EnergyInit();
    void addC9_EnergyPropagation();
    void addC10_BufferSafety();

    // Expression builders (linear in decision variables)
    GRBLinExpr buildEBlk(NodeId block);
    GRBLinExpr buildEStart(NodeId block);
    GRBLinExpr buildEEnd(NodeId block);
};

} // namespace checkpoint
