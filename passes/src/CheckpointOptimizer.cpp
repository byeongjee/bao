#include "CheckpointOptimizer.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace checkpoint {

CheckpointOptimizer::CheckpointOptimizer(const CFGAnalysis &cfg, double capacity)
    : cfg_(cfg), capacity_(capacity), env_(), model_(env_) {
    // Suppress Gurobi output by default
    model_.set(GRB_IntParam_OutputFlag, 0);
}

std::vector<std::string> CheckpointOptimizer::getInfeasibleBlocks() const {
    std::vector<std::string> infeasible;
    for (const auto &blockName : cfg_.getBlocks()) {
        const BlockInfo &info = cfg_.getBlockInfo(blockName);
        if (info.energyCost > capacity_) {
            infeasible.push_back(blockName);
        }
    }
    return infeasible;
}

bool CheckpointOptimizer::solve() {
    // Check feasibility first
    auto infeasible = getInfeasibleBlocks();
    if (!infeasible.empty()) {
        llvm::errs() << "Error: Blocks exceed capacity\n";
        return false;
    }

    buildModel();
    model_.optimize();
    solved_ = true;

    if (model_.get(GRB_IntAttr_Status) != GRB_OPTIMAL) {
        llvm::errs() << "Optimization failed: no optimal solution found\n";
        return false;
    }
    return true;
}

std::set<std::string> CheckpointOptimizer::getCheckpoints() const {
    std::set<std::string> checkpoints;
    if (!solved_) {
        return checkpoints;
    }

    for (const auto &[blockName, var] : x_) {
        if (var.get(GRB_DoubleAttr_X) > 0.5) {
            checkpoints.insert(blockName);
        }
    }
    return checkpoints;
}

double CheckpointOptimizer::getObjectiveValue() const {
    if (!solved_) {
        return 0.0;
    }
    return model_.get(GRB_DoubleAttr_ObjVal);
}

std::map<std::string, double> CheckpointOptimizer::getEnergyLevels() const {
    std::map<std::string, double> levels;
    if (!solved_) {
        return levels;
    }

    for (const auto &[blockName, var] : y_) {
        levels[blockName] = var.get(GRB_DoubleAttr_X);
    }
    return levels;
}

void CheckpointOptimizer::buildModel() {
    addVariables();
    addObjective();
    addConstraints();
    model_.update();
}

void CheckpointOptimizer::addVariables() {
    for (const auto &blockName : cfg_.getBlocks()) {
        // x[b]: binary checkpoint variable
        x_[blockName] = model_.addVar(0.0, 1.0, 0.0, GRB_BINARY,
                                       "x_" + blockName);

        // y[b]: continuous energy variable [0, capacity]
        y_[blockName] = model_.addVar(0.0, capacity_, 0.0, GRB_CONTINUOUS,
                                       "y_" + blockName);
    }
    model_.update();
}

void CheckpointOptimizer::addObjective() {
    GRBLinExpr objective = 0;
    for (const auto &blockName : cfg_.getBlocks()) {
        const BlockInfo &info = cfg_.getBlockInfo(blockName);
        objective += info.freq * x_[blockName];
    }
    model_.setObjective(objective, GRB_MINIMIZE);
}

void CheckpointOptimizer::addConstraints() {
    // Compute Big-M constant
    double maxEnergy = 0.0;
    for (const auto &blockName : cfg_.getBlocks()) {
        const BlockInfo &info = cfg_.getBlockInfo(blockName);
        maxEnergy = std::max(maxEnergy, info.energyCost);
    }
    double bigM = capacity_ + maxEnergy + 1;

    // Constraint 1: Energy propagation for each edge
    // y[v] >= y[u] + energy[u] - M * x[v]
    for (const auto &[src, dst] : cfg_.getEdges()) {
        const BlockInfo &srcInfo = cfg_.getBlockInfo(src);
        model_.addConstr(
            y_[dst] >= y_[src] + srcInfo.energyCost - bigM * x_[dst],
            "propagate_" + src + "_to_" + dst);
    }

    // Constraint 2: Capacity check for each block
    // y[b] <= capacity * (1 - x[b])
    for (const auto &blockName : cfg_.getBlocks()) {
        model_.addConstr(
            y_[blockName] <= capacity_ * (1 - x_[blockName]),
            "capacity_" + blockName);
    }

    // Constraint 3: Entry block energy is 0
    const std::string &entry = cfg_.getEntryBlock();
    if (!entry.empty() && y_.count(entry)) {
        model_.addConstr(y_[entry] == 0, "entry_energy");
    }

    // Constraint 4: Exit block completion
    // y[b] + Cost(b) <= capacity for all exit blocks
    for (const auto &exitBlock : cfg_.getExitBlocks()) {
        const BlockInfo &info = cfg_.getBlockInfo(exitBlock);
        model_.addConstr(
            y_[exitBlock] + info.energyCost <= capacity_,
            "exit_" + exitBlock);
    }
}

} // namespace checkpoint
