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

    /// DefSite IDs where store_enabled = 1.
    std::set<unsigned> enabledDefStores;

    /// VMObj placement: true = VM (SRAM), false = NVM (FRAM).
    std::map<llvm::GlobalVariable *, bool> vmPlacement;

    /// needs_vol_restore[b,v] = true: VMObj v needs volatile restore at block b.
    std::map<std::pair<std::string, unsigned>, bool> needVolRestore;

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
/// Implements the spec formulation with:
/// - Distributed checkpoint stores at definition sites
/// - VM/NVM memory placement for globals
/// - NeedVol linearization linking placement and boundary decisions
/// - Richer energy objective minimizing expected energy overhead
/// - Constraints C1, C3-C9
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
    const MILPInput &input_;
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const EnergyModel &energy_;
    const MILPEnergyParams &params_;

    GRBEnv env_;
    GRBModel model_;
    MILPSolution solution_;
    bool solved_ = false;
    bool acceptFeasible_ = false;

    // MILP variables
    std::map<std::string, GRBVar> isRegionStart_;     // x[b] binary
    std::map<unsigned, GRBVar> storeEnabled_;          // z[d] binary
    std::map<llvm::GlobalVariable *, GRBVar> placedInVm_; // p[v] binary
    // y[b,stateElemId] binary (only for VMObjs live-in at b)
    std::map<std::pair<std::string, unsigned>, GRBVar> needsVolRestore_;
    std::map<std::string, GRBVar> energyAccumulated_;  // eaccum[b] continuous

    void buildModel();
    void addVariables();
    void addObjective();
    void addConstraints();
    void extractSolution();

    // Constraint helpers
    void addC1_EntryRegionStart();
    void addC3_VMCapacity();
    void addC4_NeedVolLinearization();
    void addC5_CheckpointAvailability();
    void addC6_EnergyInit();
    void addC7_EnergyPropagation();
    void addC8_BufferSafety();

    // Expression builders for E_blk and E_start (linear in decision variables)
    GRBLinExpr buildEBlk(const std::string &block);
    GRBLinExpr buildEStart(const std::string &block);
};

} // namespace checkpoint
