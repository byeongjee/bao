#include "estimator/AssemblyBasedEstimator.h"
#include "common/BlockUtils.h"

#include "llvm/Support/raw_ostream.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>

namespace checkpoint {

std::unique_ptr<AssemblyBasedEstimator> AssemblyBasedEstimator::create(const std::string &configPath) {
    auto estimator = std::unique_ptr<AssemblyBasedEstimator>(new AssemblyBasedEstimator());
    if (!estimator->loadConfig(configPath)) {
        return nullptr;
    }
    return estimator;
}

bool AssemblyBasedEstimator::loadConfig(const std::string &configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        llvm::errs() << "Error: Cannot open config file: " << configPath << "\n";
        return false;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::errs() << "Error: JSON parse error in: " << configPath << "\n";
        return false;
    }

    // Validate required fields
    if (!config.contains("energy_parameters")) {
        llvm::errs() << "Error: Missing 'energy_parameters' in config: " << configPath << "\n";
        return false;
    }

    auto &params = config["energy_parameters"];

    // Load energy data path (required)
    if (!params.contains("energy_data_path")) {
        llvm::errs() << "Error: Missing 'energy_data_path' in config: " << configPath << "\n";
        return false;
    }
    std::string energyDataPath = params["energy_data_path"].get<std::string>();

    if (energyDataPath.empty()) {
        llvm::errs() << "Error: 'energy_data_path' is empty in config: " << configPath << "\n";
        return false;
    }

    return loadEnergyData(energyDataPath);
}

bool AssemblyBasedEstimator::loadEnergyData(const std::string &energyDataPath) {
    std::ifstream file(energyDataPath);
    if (!file.is_open()) {
        llvm::errs() << "Error: Cannot open energy data file: " << energyDataPath << "\n";
        return false;
    }

    nlohmann::json data = nlohmann::json::parse(file, nullptr, false);
    if (data.is_discarded()) {
        llvm::errs() << "Error: JSON parse error in: " << energyDataPath << "\n";
        return false;
    }

    // Parse functions
    if (!data.contains("functions")) {
        llvm::errs() << "Error: Missing 'functions' in energy data: " << energyDataPath << "\n";
        return false;
    }

    for (auto &[funcName, funcData] : data["functions"].items()) {
        if (!funcData.contains("bb_energy")) {
            llvm::errs() << "Warning: Function '" << funcName
                         << "' has no bb_energy data, skipping\n";
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
        llvm::errs() << "Warning: No function energy data loaded from: " << energyDataPath << "\n";
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
        llvm::errs() << "Warning: Function '" << currentFuncName_
                     << "' not found in energy data\n";
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
    llvm::errs() << "Warning: BB '" << bbName << "' in function '"
                 << currentFuncName_ << "' not found in energy data\n";
    return EnergyEstimate{0.0, "assembly-missing-bb"};
}

} // namespace checkpoint
