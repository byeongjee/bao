#pragma once

#include "estimator/EnergyEstimator.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace checkpoint {

/// Factory for creating energy estimators.
/// Supports registration of new estimator types for extensibility.
class EnergyEstimatorFactory {
  public:
    /// Creator function type: takes config path, returns estimator.
    using CreatorFn = std::function<EnergyEstimatorPtr(const std::string &configPath)>;

    /// Default constructor. Does not register any types.
    EnergyEstimatorFactory() = default;

    /// Create a factory with built-in estimator types registered.
    /// Currently registers: "ir" (IRBasedEstimator)
    static EnergyEstimatorFactory createDefault();

    /// Register an estimator type.
    /// @param name Type identifier (e.g., "ir", "assembly").
    /// @param creator Function to create the estimator.
    void registerType(const std::string &name, CreatorFn creator);

    /// Create estimator by explicit type name.
    /// @param type Estimator type identifier.
    /// @param configPath Path to configuration file.
    /// @return Created estimator, or nullptr if type not found.
    EnergyEstimatorPtr create(const std::string &type, const std::string &configPath) const;

    /// Create estimator from config file.
    /// Reads "estimator_type" field from config (defaults to "ir").
    /// @param configPath Path to configuration file.
    /// @return Created estimator, or nullptr on error.
    EnergyEstimatorPtr createFromConfig(const std::string &configPath) const;

    /// List all registered estimator types.
    std::vector<std::string> getRegisteredTypes() const;

  private:
    std::unordered_map<std::string, CreatorFn> creators_;
};

} // namespace checkpoint
