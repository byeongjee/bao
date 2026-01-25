#pragma once

#include "EnergyEstimator.h"

#include <string>
#include <unordered_map>

namespace checkpoint {

/// Energy estimator based on LLVM IR instruction analysis.
/// Reads cost configuration from a JSON file and sums instruction
/// costs to estimate energy for each basic block.
class IRBasedEstimator : public EnergyEstimator {
public:
    /// Construct estimator from configuration file.
    /// @param configPath Path to JSON configuration file.
    /// @throws llvm::report_fatal_error if file not found or invalid.
    explicit IRBasedEstimator(const std::string &configPath);

    EnergyEstimate estimate(const llvm::BasicBlock &BB) override;
    double getCapacity() const override;
    std::string getName() const override;

private:
    double capacity_;
    std::unordered_map<std::string, int> instructionCosts_;

    /// Load configuration from JSON file.
    void loadConfig(const std::string &path);

    /// Get cost for an LLVM opcode.
    int getInstructionCost(unsigned Opcode) const;

    /// Map opcode to cost category name.
    std::string getCostCategory(unsigned Opcode) const;
};

} // namespace checkpoint
