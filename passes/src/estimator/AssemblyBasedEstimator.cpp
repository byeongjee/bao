#include "estimator/AssemblyBasedEstimator.h"
#include "common/BlockUtils.h"
#include "common/Logger.h"

#define JSON_NOEXCEPTION
#include <fstream>
#include <nlohmann/json.hpp>

namespace checkpoint {

std::unique_ptr<AssemblyBasedEstimator>
AssemblyBasedEstimator::create(const std::string &configPath) {
    auto estimator = std::unique_ptr<AssemblyBasedEstimator>(new AssemblyBasedEstimator());
    if (!estimator->loadConfig(configPath)) {
        return nullptr;
    }
    return estimator;
}

bool AssemblyBasedEstimator::loadConfig(const std::string &configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        PLOGE << "Error: Cannot open config file: " << configPath;
        return false;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        PLOGE << "Error: JSON parse error in: " << configPath;
        return false;
    }

    // Validate required fields
    if (!config.contains("energy_parameters")) {
        PLOGE << "Error: Missing 'energy_parameters' in config: " << configPath;
        return false;
    }

    auto &params = config["energy_parameters"];

    // Load energy data path (required)
    if (!params.contains("energy_data_path")) {
        PLOGE << "Error: Missing 'energy_data_path' in config: " << configPath;
        return false;
    }
    std::string energyDataPath = params["energy_data_path"].get<std::string>();

    if (energyDataPath.empty()) {
        PLOGE << "Error: 'energy_data_path' is empty in config: " << configPath;
        return false;
    }

    return loadEnergyData(energyDataPath);
}

bool AssemblyBasedEstimator::loadEnergyData(const std::string &energyDataPath) {
    std::ifstream file(energyDataPath);
    if (!file.is_open()) {
        PLOGE << "Error: Cannot open energy data file: " << energyDataPath;
        return false;
    }

    nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
    if (data.is_discarded()) {
        PLOGE << "Error: JSON parse error in: " << energyDataPath;
        return false;
    }

    // Parse functions
    if (!data.contains("functions")) {
        PLOGE << "Error: Missing 'functions' in energy data: " << energyDataPath;
        return false;
    }

    for (auto &[funcName, funcData] : data["functions"].items()) {
        if (!funcData.contains("bb_energy")) {
            PLOGW << "Warning: Function '" << funcName << "' has no bb_energy data, skipping";
            continue;
        }

        std::map<std::string, double> bbEnergies;
        for (auto &[bbName, bbData] : funcData["bb_energy"].items()) {
            double energy = 0.0;
            if (bbData.contains("energy")) {
                energy = bbData["energy"].get<double>();
            }
            bbEnergies[bbName] = std::move(energy);
        }
        functionEnergy_[funcName] = std::move(bbEnergies);
    }

    if (functionEnergy_.empty()) {
        PLOGW << "Warning: No function energy data loaded from: " << energyDataPath;
    }

    return true;
}

std::string AssemblyBasedEstimator::getName() const {
    return "assembly-based";
}

void AssemblyBasedEstimator::prepareForFunction(const llvm::Function &F) {
    currentFunction_ = &F;
    currentFuncName_ = F.getName().str();

    auto it = functionEnergy_.find(currentFuncName_);
    functionFound_ = (it != functionEnergy_.end());

    if (!functionFound_) {
        PLOGW << "Warning: Function '" << currentFuncName_ << "' not found in energy data";
    }
}

void AssemblyBasedEstimator::finalizeFunction(const llvm::Function &F) {
    currentFunction_ = nullptr;
    currentFuncName_.clear();
    functionFound_ = false;
}

EnergyEstimate AssemblyBasedEstimator::estimate(const llvm::BasicBlock &BB) {
    // If prepareForFunction wasn't called, call it now
    const llvm::Function *F = BB.getParent();
    if (currentFunction_ != F) {
        prepareForFunction(*F);
    }

    if (!functionFound_) {
        // Function not in energy data - return 0 with warning method
        return EnergyEstimate{0.0, "assembly-missing-function"};
    }

    std::string bbName = getBlockName(BB, *F);

    auto &funcEnergy = functionEnergy_[currentFuncName_];
    auto it = funcEnergy.find(bbName);

    if (it != funcEnergy.end()) {
        return EnergyEstimate{it->second, "assembly-lookup"};
    }

    // BB not found in energy data - likely no assembly code for this BB
    PLOGW << "Warning: BB '" << bbName << "' in function '" << currentFuncName_
          << "' not found in energy data";
    return EnergyEstimate{0.0, "assembly-missing-bb"};
}

} // namespace checkpoint
