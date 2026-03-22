#include "EnergyModel.h"

#include "llvm/Support/raw_ostream.h"

#include "common/Logger.h"

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
        PLOGE << "error: failed to open energy config file '" << configPath << "'";
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

        PLOGI << "Loaded " << costs_.size() << " energy cost entries from '" << configPath << "'";

        // Load ignored call targets (benchmark infrastructure calls with zero energy cost)
        if (config.contains("ignored_call_targets")) {
            for (const auto &name : config["ignored_call_targets"]) {
                ignoredCallTargets_.insert(name.get<std::string>());
            }
            PLOGI << "Loaded " << ignoredCallTargets_.size() << " ignored call targets";
        }

    } catch (const json::exception &e) {
        PLOGE << "error: failed to parse energy config '" << configPath << "': " << e.what();
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
    requiredKeys_.insert(key);

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
        PLOGW << "warning: no energy cost for '" << key << "', using default (" << defaultEnergy_
              << ")";
        warned[key] = true;
    }

    missingKeys_.insert(key);
    return defaultEnergy_;
}

double EnergyModel::getCallEnergy(const std::string &addrMode,
                                  const std::string &callTarget) const {
    // Whitelisted calls contribute zero energy
    if (!callTarget.empty() && ignoredCallTargets_.count(callTarget)) {
        return 0.0;
    }

    // If we have a resolved target, use call_{function_name} as primary key
    if (!callTarget.empty()) {
        std::string primaryKey = "call_" + callTarget;
        requiredKeys_.insert(primaryKey);

        auto it = costs_.find(primaryKey);
        if (it != costs_.end()) {
            return it->second;
        }

        // Fallback 1: base function name (e.g., call_memcpy_16 -> call_memcpy)
        auto underscorePos = callTarget.rfind('_');
        if (underscorePos != std::string::npos) {
            std::string baseName = callTarget.substr(0, underscorePos);
            std::string baseKey = "call_" + baseName;
            it = costs_.find(baseKey);
            if (it != costs_.end()) {
                PLOGW << "WARNING: no energy cost for '" << primaryKey << "', falling back to '"
                      << baseKey << "' (" << it->second << ")";
                return it->second;
            }
        }

        // Fallback 2: call_{addrMode} (e.g., call_immediate)
        std::string fallback2 = makeKey("call", addrMode);
        it = costs_.find(fallback2);
        if (it != costs_.end()) {
            PLOGW << "WARNING: no energy cost for '" << primaryKey << "', falling back to '"
                  << fallback2 << "' (" << it->second << ")";
            return it->second;
        }

        // Fallback 3: default energy
        PLOGW << "WARNING: no energy cost for '" << primaryKey
              << "', no fallback found, using default (" << defaultEnergy_ << ")";
        missingKeys_.insert(primaryKey);
        return defaultEnergy_;
    }

    // No resolved target (indirect call) — use existing call_{addrMode} path
    return getEnergy("call", addrMode);
}

} // namespace bbanalyzer
