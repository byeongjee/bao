#pragma once

#include "common/ValueOrder.h"
#include "milp/ModelViews.h"

#include "gurobi_c++.h"

#include <map>
#include <memory>
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

/// Why solve() returned false. Distinguishes proven infeasibility from
/// solver limits so callers do not report a timeout as "infeasible".
enum class SolveFailure {
    None,                 ///< solve() has not failed.
    BlocksExceedCapacity, ///< A block's base energy exceeds capacity (pre-check).
    ProvenInfeasible,     ///< Gurobi proved the model infeasible.
    TimeLimitNoSolution,  ///< Time limit expired before any feasible solution was found.
    FeasibleNotAccepted,  ///< Feasible incumbent found, but optimality unproven and
                          ///< accept-feasible is off.
    SolverError,          ///< Gurobi error or unexpected solver status.
};

/// Solution from the MILP optimizer.
struct MILPSolution {
    /// r_b: nodes where region start = 1.
    std::set<NodeId> r;

    /// m_{b,v}: VM placement for all tracked variables.
    std::map<std::pair<NodeId, llvm::Value *>, bool> m;

    /// r̂_{b,v}: need-restore indicator for all tracked variables.
    std::map<std::pair<NodeId, llvm::Value *>, bool> rHat;

    /// s_{b,v}: save indicator (for b != b0 and v in LiveIn(b)).
    std::map<std::pair<NodeId, llvm::Value *>, bool> s;

    /// ε_accum[b]: accumulated energy at block b.
    std::map<NodeId, double> eAccum;

    /// Objective function value.
    double objectiveValue = 0.0;

    /// Solver optimality status.
    SolverStatus solverStatus = SolverStatus::Optimal;

    /// MIP optimality gap (0.0 for optimal, >0 for feasible).
    double mipGap = 0.0;

    /// Get all Value*s with s=true at a given node.
    std::vector<llvm::Value *> getSaveVarsAt(NodeId node) const {
        std::vector<llvm::Value *> result;
        for (const auto &[key, enabled] : s)
            if (enabled && key.first == node)
                result.push_back(key.second);
        stableSortAndUniqueValues(result);
        return result;
    }

    /// Get all Value*s with rHat=true at a given node.
    std::vector<llvm::Value *> getRestoreVarsAt(NodeId node) const {
        std::vector<llvm::Value *> result;
        for (const auto &[key, enabled] : rHat)
            if (enabled && key.first == node)
                result.push_back(key.second);
        stableSortAndUniqueValues(result);
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

/// Problem size statistics for the MILP model before and after Gurobi presolve.
struct MILPProblemSizeStats {
    int variablesBeforePresolve = 0;
    int constraintsBeforePresolve = 0;
    int variablesAfterPresolve = 0;
    int constraintsAfterPresolve = 0;
};

/// MILP optimizer for checkpoint placement using Gurobi.
class CheckpointOptimizer {
  public:
    CheckpointOptimizer(const MILPInput &input);

    bool solve();

    void setCoarseAllocation(bool enable) { coarseAllocation_ = enable; }
    void setAcceptFeasible(bool accept) { acceptFeasible_ = accept; }
    void setTimeLimit(double seconds) { timeLimit_ = seconds; }
    void setMIPGap(double gap) { mipGap_ = gap; }
    void setLogFile(const std::string &path) { logFile_ = path; }
    bool isCoarseAllocationEnabled() const { return coarseAllocation_; }

    const MILPSolution &getSolution() const { return solution_; }

    /// Why the last solve() call failed (SolveFailure::None if it succeeded).
    SolveFailure getSolveFailure() const { return solveFailure_; }

    std::set<NodeId> getCheckpoints() const { return solution_.r; }

    double getObjectiveValue() const { return solution_.objectiveValue; }

    const MILPProblemSizeStats &getProblemSizeStats();
    int getNumVars();
    int getNumConstrs();
    int getNumPresolvedVars();
    int getNumPresolvedConstrs();

    /// Check feasibility - returns nodes whose base energy exceeds capacity.
    std::vector<NodeId> getInfeasibleBlocks() const;

  private:
    const ICFGView &cfg_;
    const IStateView &state_;
    const IEnergyView &energy_;
    const MILPEnergyParams &params_;

    /// Gurobi objects are created lazily in ensureModelBuilt() behind a
    /// try/catch: GRBEnv construction throws on e.g. license errors, and
    /// every caller of this class builds with -fno-exceptions. This
    /// translation unit alone compiles with -fexceptions (see
    /// passes/CMakeLists.txt) so Gurobi failures become error returns
    /// instead of std::terminate.
    std::unique_ptr<GRBEnv> env_;
    std::unique_ptr<GRBModel> model_;
    MILPSolution solution_;
    MILPProblemSizeStats problemSizeStats_;
    SolveFailure solveFailure_ = SolveFailure::None;
    bool solved_ = false;
    bool modelBuilt_ = false;
    bool gurobiFailed_ = false;
    bool baseSizeStatsComputed_ = false;
    bool presolveStatsComputed_ = false;
    bool coarseAllocation_ = false;
    bool acceptFeasible_ = false;
    double timeLimit_ = 600.0;
    double mipGap_ = 0.0;
    std::string logFile_;

    using BlockVarKey = std::pair<NodeId, llvm::Value *>;

    // MILP variables (paper notation)
    std::map<NodeId, GRBVar> r_;                       // r_b: region start
    std::map<BlockVarKey, GRBVar> m_;                  // m_{b,v}: VM placement (fine mode)
    std::map<llvm::GlobalVariable *, GRBVar> mGlobal_; // m_v: VM placement (coarse mode)
    std::map<BlockVarKey, GRBVar> rHat_;               // r̂_{b,v}: need-restore (all tracked)
    std::map<BlockVarKey, GRBVar> d_;                  // d_{b,v}: dirty/pending (all tracked)
    std::map<BlockVarKey, GRBVar> s_;                  // s_{b,v}: save (all tracked)
    std::map<NodeId, GRBVar> eAccum_;                  // ε_accum[b]: accumulated energy

    std::map<NodeId, std::vector<NodeId>> predecessors_;
    std::vector<llvm::GlobalVariable *> orderedVmObjs_;
    std::vector<llvm::Value *> orderedTrackedValues_;
    std::vector<BlockVarKey> rHatKeys_;
    std::vector<BlockVarKey> sKeys_;

    /// Reach(v): set of blocks where d_{b,v} can be non-zero.
    /// Computed as forward-reachable closure from {b : D_{b,v} = 1}.
    std::map<llvm::Value *, std::set<NodeId>> reachableDefs_;

    /// Returns false if Gurobi initialization or model construction failed.
    bool ensureModelBuilt();
    void computeProblemSizeStats();
    void buildModel();
    void addVariables();
    void addObjective();
    void addConstraints();
    void extractSolution();

    // Constraint helpers (paper constraint numbers)
    void constrainEntryAsRegionStart();       // C1
    void constrainIneligiblePlacement();      // C2 (new)
    void constrainNeedRestoreLinearization(); // C3
    void constrainVMCapacity();               // VM capacity
    void constrainPlacementPropagation();     // C12, C13
    void constrainDirtyPropagation();         // C4, C5, C6
    void constrainSaveAtRegionBoundary();     // C7, C8
    void constrainEnergyInitAtRegionStart();  // C9
    void constrainEnergyPropagation();        // C10
    void constrainEnergyWithinCapacity();     // C11

    // Expression builders (linear in decision variables)
    GRBLinExpr buildEBlk(NodeId block);
    GRBLinExpr buildEStart(NodeId block);
    GRBLinExpr buildEEnd(NodeId block);
};

} // namespace checkpoint
