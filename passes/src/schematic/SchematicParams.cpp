#include "schematic/SchematicParams.h"

#include "llvm/Support/raw_ostream.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace checkpoint {

std::optional<SchematicParams> parseSchematicParams(const std::string &configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        llvm::errs() << "Error: Cannot open SCHEMATIC config file: " << configPath << "\n";
        return std::nullopt;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::errs() << "Error: JSON parse error in SCHEMATIC config: " << configPath << "\n";
        return std::nullopt;
    }

    const std::vector<std::string> requiredDouble = {"capacity",
                                                     "E_pro",
                                                     "E_epi",
                                                     "reg_store_energy",
                                                     "reg_restore_energy",
                                                     "nvm_access_penalty",
                                                     "mem_store_energy_per_byte",
                                                     "mem_restore_energy_per_byte",
                                                     "loop_increment_cost_nvm"};

    for (const auto &field : requiredDouble) {
        if (!config.contains(field)) {
            llvm::errs() << "Error: Missing required field '" << field
                         << "' in SCHEMATIC config: " << configPath << "\n";
            return std::nullopt;
        }
    }

    const std::vector<std::string> requiredUnsigned = {"N_reg", "vm_capacity_bytes"};
    for (const auto &field : requiredUnsigned) {
        if (!config.contains(field)) {
            llvm::errs() << "Error: Missing required field '" << field
                         << "' in SCHEMATIC config: " << configPath << "\n";
            return std::nullopt;
        }
    }

    // SCHEMATIC-specific section (unified format) or root (legacy).
    nlohmann::json schSection;
    if (config.contains("schematic") && config["schematic"].is_object())
        schSection = config["schematic"];

    SchematicParams params;
    params.capacity = config["capacity"].get<double>();
    params.E_pro = config["E_pro"].get<double>();
    params.E_epi = config["E_epi"].get<double>();
    params.N_reg = config["N_reg"].get<unsigned>();
    params.regStoreEnergy = config["reg_store_energy"].get<double>();
    params.regRestoreEnergy = config["reg_restore_energy"].get<double>();
    params.nvmAccessPenalty = config["nvm_access_penalty"].get<double>();
    params.memStoreEnergyPerByte = config["mem_store_energy_per_byte"].get<double>();
    params.memRestoreEnergyPerByte = config["mem_restore_energy_per_byte"].get<double>();
    params.vmCapacityBytes = config["vm_capacity_bytes"].get<unsigned>();
    params.loopIncrementCostNvm = config["loop_increment_cost_nvm"].get<double>();

    // max_paths: check schematic section first, then root.
    if (schSection.contains("max_paths")) {
        params.maxPaths = schSection["max_paths"].get<unsigned>();
    } else if (config.contains("max_paths")) {
        params.maxPaths = config["max_paths"].get<unsigned>();
    } else {
        llvm::errs() << "Error: Missing required field 'max_paths'"
                     << " in SCHEMATIC config: " << configPath << "\n";
        return std::nullopt;
    }

    // Helper to read bool from schematic section or root.
    auto readBool = [&](const std::string &key, bool defaultVal) -> std::optional<bool> {
        for (const auto *src : {&schSection, &config}) {
            if (src->contains(key)) {
                if (!(*src)[key].is_boolean()) {
                    llvm::errs() << "Error: Field '" << key << "' must be boolean"
                                 << " in SCHEMATIC config: " << configPath << "\n";
                    return std::nullopt;
                }
                return (*src)[key].get<bool>();
            }
        }
        return defaultVal;
    };

    auto debugMarkers = readBool("add_debug_markers", false);
    if (!debugMarkers)
        return std::nullopt;
    params.addDebugMarkers = *debugMarkers;

    return params;
}

} // namespace checkpoint
