#pragma once

#include "estimator/EnergyEstimator.h"

#include <string>
#include <unordered_map>

namespace checkpoint {

/// Energy estimator based on LLVM IR instruction analysis.
/// Reads cost configuration from a JSON file and sums instruction
/// costs to estimate energy for each basic block.
class IRBasedEstimator : public EnergyEstimator {
  public:
    /// Create estimator from configuration file.
    /// @param configPath Path to JSON configuration file.
    /// @return Estimator instance, or nullptr on error (with message to errs()).
    static std::unique_ptr<IRBasedEstimator> create(const std::string &configPath);

    EnergyEstimate estimate(const llvm::BasicBlock &BB) override;
    std::string getName() const override;
    double getInstructionCost(const llvm::Instruction &I) override;

  private:
    /// Private constructor - use create() factory method.
    IRBasedEstimator() = default;

    std::unordered_map<std::string, double> instructionCosts_;

    /// Load configuration from JSON file.
    /// @return true on success, false on error (with message to errs()).
    bool loadConfig(const std::string &path);

    /// Get cost for an LLVM opcode.
    double getInstructionCost(unsigned Opcode) const;

    /// Map opcode to cost category name.
    std::string getCostCategory(unsigned Opcode) const;
};

} // namespace checkpoint
