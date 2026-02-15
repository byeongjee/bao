#pragma once

#include "common/CFGAnalysis.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include "gurobi_c++.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Input data for the MILP optimizer (aggregates all analysis results).
struct MILPInput {
    const CFGAnalysis &cfg;
    const StateAnalysis &state;
    const EnergyModel &energy;
};

/// Solver optimality status.
enum class SolverStatus {
    Optimal,  ///< Proven optimal solution.
    Feasible, ///< Feasible but not proven optimal (e.g., time limit).
};

/// Solution from the MILP optimizer.
struct MILPSolution {
    /// Blocks where is_region_start = 1.
    std::set<std::string> regionStarts;

    /// place_in_vm[b,v] values.
    std::map<std::pair<std::string, llvm::GlobalVariable *>, bool> placeInVm;

    /// need_restore[b,v] values (for v in LiveIn(b)).
    std::map<std::pair<std::string, llvm::GlobalVariable *>, bool> needRestore;

    /// commit[b,v] values (for b != b0 and v in LiveIn(b)).
    std::map<std::pair<std::string, llvm::GlobalVariable *>, bool> commit;

    /// energy_accumulated[b] values.
    std::map<std::string, double> energyAccumulated;

    /// Objective function value.
    double objectiveValue = 0.0;

    /// Solver optimality status.
    SolverStatus solverStatus = SolverStatus::Optimal;

    /// MIP optimality gap (0.0 for optimal, >0 for feasible).
    double mipGap = 0.0;
};

/// MILP optimizer for checkpoint placement using Gurobi.
///
/// Implements the deterministic MILP formulation with boundary commit
/// decisions and per-block placement variables.
class CheckpointOptimizer {
public:
    /// Construct optimizer from analysis results.
    CheckpointOptimizer(const MILPInput &input);

    /// Build and solve the MILP model.
    /// @return true if optimization succeeded, false otherwise.
    bool solve();

    /// Allow accepting feasible (non-optimal) solutions.
    /// When true, solutions from time/node/solution limits are accepted
    /// if Gurobi found at least one feasible solution.
    void setAcceptFeasible(bool accept) { acceptFeasible_ = accept; }

    /// Get the full MILP solution.
    const MILPSolution &getSolution() const { return solution_; }

    /// Get blocks where region starts are placed (convenience, same as
    /// solution.regionStarts).
    std::set<std::string> getCheckpoints() const {
        return solution_.regionStarts;
    }

    /// Get the objective value.
    double getObjectiveValue() const { return solution_.objectiveValue; }

    /// Get number of MILP variables in the model.
    int getNumVars() const;

    /// Get number of MILP constraints in the model.
    int getNumConstrs() const;

    /// Check feasibility - returns blocks whose base energy exceeds capacity.
    std::vector<std::string> getInfeasibleBlocks() const;

private:
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const EnergyModel &energy_;
    const MILPEnergyParams &params_;

    GRBEnv env_;
    GRBModel model_;
    MILPSolution solution_;
    bool solved_ = false;
    bool acceptFeasible_ = false;

    using BlockGVKey = std::pair<std::string, llvm::GlobalVariable *>;

    // MILP variables
    std::map<std::string, GRBVar> isRegionStart_; // x[b]
    std::map<BlockGVKey, GRBVar> placeInVm_;      // p[b,v]
    std::map<BlockGVKey, GRBVar> needRestore_;    // y[b,v]
    std::map<BlockGVKey, GRBVar> pending_;        // pending[b,v]
    std::map<BlockGVKey, GRBVar> vmPending_;      // vm_pending[b,v]
    std::map<BlockGVKey, GRBVar> commit_;         // commit[b,v]
    std::map<std::string, GRBVar> energyAccumulated_; // eaccum[b]

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
    GRBLinExpr buildEBlk(const std::string &block);
    GRBLinExpr buildEStart(const std::string &block);
    GRBLinExpr buildEEnd(const std::string &block);
};

} // namespace checkpoint
