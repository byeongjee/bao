#include "estimator/EnergyEstimatorFactory.h"
#include "common/Logger.h"
#include "estimator/AssemblyBasedEstimator.h"
#include "estimator/IRBasedEstimator.h"

#define JSON_NOEXCEPTION
#include <fstream>
#include <nlohmann/json.hpp>

namespace checkpoint {

EnergyEstimatorFactory EnergyEstimatorFactory::createDefault() {
    EnergyEstimatorFactory factory;
    // Register built-in IR-based estimator
    factory.registerType(
        "ir", [](const std::string &configPath) { return IRBasedEstimator::create(configPath); });
    // Register assembly-based estimator
    factory.registerType("assembly", [](const std::string &configPath) {
        return AssemblyBasedEstimator::create(configPath);
    });
    return factory;
}

void EnergyEstimatorFactory::registerType(const std::string &name, CreatorFn creator) {
    creators_[name] = std::move(creator);
}

EnergyEstimatorPtr EnergyEstimatorFactory::create(const std::string &type,
                                                  const std::string &configPath) const {
    auto it = creators_.find(type);
    if (it == creators_.end()) {
        return nullptr;
    }
    return it->second(configPath);
}

EnergyEstimatorPtr EnergyEstimatorFactory::createFromConfig(const std::string &configPath) const {
    // Read config to determine estimator type
    std::ifstream file(configPath);
    if (!file.is_open()) {
        PLOGE << "Error: Cannot open energy config file: " << configPath;
        return nullptr;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        PLOGE << "Error: JSON parse error in: " << configPath;
        return nullptr;
    }

    // Get estimator type (required field)
    if (!config.contains("estimator_type")) {
        PLOGE << "Error: Missing required 'estimator_type' field in config: " << configPath
              << " Valid types: ir, assembly";
        return nullptr;
    }
    std::string estimatorType = config["estimator_type"].get<std::string>();

    // Create the estimator
    auto estimator = create(estimatorType, configPath);
    if (!estimator) {
        PLOGE << "Error: Unknown or failed estimator type '" << estimatorType
              << "' in config: " << configPath;
        return nullptr;
    }

    return estimator;
}

std::vector<std::string> EnergyEstimatorFactory::getRegisteredTypes() const {
    std::vector<std::string> types;
    types.reserve(creators_.size());
    for (const auto &[name, creator] : creators_) {
        types.push_back(name);
    }
    return types;
}

} // namespace checkpoint
