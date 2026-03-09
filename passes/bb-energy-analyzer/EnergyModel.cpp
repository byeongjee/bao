#include "EnergyModel.h"

#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <nlohmann/json.hpp>

using namespace llvm;
using json = nlohmann::json;

namespace bbanalyzer {

EnergyModel::EnergyModel(const std::string &configPath) {
    if (configPath.empty())
        return;

    std::ifstream file(configPath);
    if (!file.is_open()) {
        errs() << "error: failed to open energy config file '" << configPath << "'\n";
        return;
    }

    try {
        json config = json::parse(file);

        // Load default energy if specified
        if (config.contains("default_energy")) {
            defaultEnergy_ = config["default_energy"].get<double>();
        }

        // Load instruction costs from "parameters" object
        if (config.contains("parameters")) {
            const auto &params = config["parameters"];
            for (auto it = params.begin(); it != params.end(); ++it) {
                costs_[it.key()] = it.value().get<double>();
            }
        }

        errs() << "Loaded " << costs_.size() << " energy cost entries from '" << configPath
               << "'\n";

    } catch (const json::exception &e) {
        errs() << "error: failed to parse energy config '" << configPath << "': " << e.what()
               << "\n";
    }
}

std::string EnergyModel::makeKey(const std::string &mnemonic, const std::string &addrMode) {
    if (addrMode.empty()) {
        return mnemonic;
    }
    return mnemonic + "_" + addrMode;
}

bool EnergyModel::hasEnergy(const std::string &mnemonic, const std::string &addrMode) const {
    return costs_.find(makeKey(mnemonic, addrMode)) != costs_.end();
}

double EnergyModel::getEnergy(const std::string &mnemonic, const std::string &addrMode) const {
    std::string key = makeKey(mnemonic, addrMode);

    auto it = costs_.find(key);
    if (it != costs_.end()) {
        return it->second;
    }

    // Try without addressing mode (for single-operand instructions)
    it = costs_.find(mnemonic);
    if (it != costs_.end()) {
        return it->second;
    }

    // Emit warning for unknown instruction/mode combo
    static std::unordered_map<std::string, bool> warned;
    if (warned.find(key) == warned.end()) {
        errs() << "warning: no energy cost for '" << key << "', using default (" << defaultEnergy_
               << ")\n";
        warned[key] = true;
    }

    return defaultEnergy_;
}

} // namespace bbanalyzer
