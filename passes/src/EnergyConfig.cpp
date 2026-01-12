#include "EnergyConfig.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/ADT/Twine.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>

namespace checkpoint {

bool EnergyConfig::loaded_ = false;
double EnergyConfig::capacity_ = 0.0;
std::unordered_map<std::string, int> EnergyConfig::instructionCosts_;

void EnergyConfig::loadFromFile(const std::string &path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        llvm::report_fatal_error(llvm::Twine("Cannot open energy config file: ") + path);
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::report_fatal_error(llvm::Twine("JSON parse error in: ") + path);
    }

    // Validate required fields
    if (!config.contains("energy_parameters")) {
        llvm::report_fatal_error(llvm::Twine("Missing 'energy_parameters' in config: ") + path);
    }

    auto &params = config["energy_parameters"];

    // Load capacity (required)
    if (!params.contains("capacity")) {
        llvm::report_fatal_error(llvm::Twine("Missing 'capacity' in config: ") + path);
    }
    capacity_ = params["capacity"].get<double>();

    // Load instruction costs (required)
    if (!params.contains("instruction_costs")) {
        llvm::report_fatal_error(llvm::Twine("Missing 'instruction_costs' in config: ") + path);
    }

    auto &costs = params["instruction_costs"];

    // Required cost categories
    const std::vector<std::string> requiredCategories = {
        "simple_arithmetic", "complex_arithmetic", "floating_point",
        "load", "store", "control_flow", "comparison", "conversion",
        "call", "phi_select", "gep", "alloca", "atomic", "default"
    };

    for (const auto &cat : requiredCategories) {
        if (!costs.contains(cat)) {
            llvm::report_fatal_error(llvm::Twine("Missing instruction cost category '") +
                                     cat + "' in config: " + path);
        }
        instructionCosts_[cat] = costs[cat].get<int>();
    }

    loaded_ = true;
}

bool EnergyConfig::isLoaded() {
    return loaded_;
}

int EnergyConfig::getInstructionCost(const std::string &category) {
    if (!loaded_) {
        llvm::report_fatal_error(
            "EnergyConfig not loaded. Provide --energy-config option.");
    }
    auto it = instructionCosts_.find(category);
    if (it != instructionCosts_.end()) {
        return it->second;
    }
    return instructionCosts_["default"];
}

double EnergyConfig::getCapacity() {
    if (!loaded_) {
        llvm::report_fatal_error(
            "EnergyConfig not loaded. Provide --energy-config option.");
    }
    return capacity_;
}

} // namespace checkpoint
