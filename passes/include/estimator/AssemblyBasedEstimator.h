#pragma once

#include "estimator/EnergyEstimator.h"

#include <map>
#include <string>

namespace checkpoint {

/// Energy estimator using pre-computed assembly-level energy costs.
/// Reads energy data from a JSON file produced by bb-energy-analyzer.
class AssemblyBasedEstimator : public EnergyEstimator {
public:
    /// Create estimator from configuration file.
    /// Config must have "estimator_type": "assembly" and
    /// "energy_parameters.energy_data_path" pointing to the energy data JSON.
    /// @param configPath Path to JSON configuration file.
    /// @return Estimator instance, or nullptr on error (with message to errs()).
    static std::unique_ptr<AssemblyBasedEstimator> create(const std::string &configPath);

    EnergyEstimate estimate(const llvm::BasicBlock &BB) override;
    std::string getName() const override;
    void prepareForFunction(const llvm::Function &F) override;
    void finalizeFunction(const llvm::Function &F) override;

private:
    AssemblyBasedEstimator() = default;

    // Per-function, per-BB energy: funcName -> {bbName -> energy}
    std::map<std::string, std::map<std::string, double>> functionEnergy_;

    // Current function state
    const llvm::Function *currentFunction_ = nullptr;
    std::string currentFuncName_;
    bool functionFound_ = false;

    bool loadConfig(const std::string &configPath);
    bool loadEnergyData(const std::string &energyDataPath);
};

} // namespace checkpoint
