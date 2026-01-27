#include "EnergyEstimatorFactory.h"
#include "IRBasedEstimator.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/Twine.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>

namespace checkpoint {

EnergyEstimatorFactory EnergyEstimatorFactory::createDefault() {
    EnergyEstimatorFactory factory;
    // Register built-in IR-based estimator
    factory.registerType("ir", [](const std::string &configPath) {
        return std::make_unique<IRBasedEstimator>(configPath);
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
        llvm::report_fatal_error(llvm::Twine("Cannot open energy config file: ") + configPath);
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::report_fatal_error(llvm::Twine("JSON parse error in: ") + configPath);
    }

    // Get estimator type (required field)
    if (!config.contains("estimator_type")) {
        llvm::report_fatal_error(llvm::Twine("Missing required 'estimator_type' field in config: ") +
                                 configPath + "\nValid types: ir");
    }
    std::string estimatorType = config["estimator_type"].get<std::string>();

    // Create the estimator
    auto estimator = create(estimatorType, configPath);
    if (!estimator) {
        llvm::report_fatal_error(llvm::Twine("Unknown estimator type '") +
                                 estimatorType + "' in config: " + configPath);
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
