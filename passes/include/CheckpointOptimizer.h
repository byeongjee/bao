#pragma once

#include "CFGAnalysis.h"

#include "gurobi_c++.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// MILP optimizer for checkpoint placement using Gurobi.
/// Minimizes frequency-weighted checkpoint count while ensuring
/// energy between checkpoints never exceeds capacity.
class CheckpointOptimizer {
public:
    /// Construct optimizer for a CFG.
    /// @param cfg The CFG analysis results.
    /// @param capacity Maximum energy capacity between checkpoints.
    CheckpointOptimizer(const CFGAnalysis &cfg, double capacity);

    /// Build and solve the MILP model.
    /// @return true if optimization succeeded, false otherwise.
    bool solve();

    /// Get the set of blocks where checkpoints should be placed.
    std::set<std::string> getCheckpoints() const;

    /// Get the objective value (total frequency-weighted checkpoint cost).
    double getObjectiveValue() const;

    /// Get the accumulated energy at the start of each block.
    std::map<std::string, double> getEnergyLevels() const;

    /// Check feasibility - returns blocks that exceed capacity.
    std::vector<std::string> getInfeasibleBlocks() const;

private:
    const CFGAnalysis &cfg_;
    double capacity_;

    GRBEnv env_;
    GRBModel model_;
    std::map<std::string, GRBVar> x_;  // Binary: checkpoint at block
    std::map<std::string, GRBVar> y_;  // Continuous: energy level

    bool solved_ = false;

    void buildModel();
    void addVariables();
    void addObjective();
    void addConstraints();
};

} // namespace checkpoint
