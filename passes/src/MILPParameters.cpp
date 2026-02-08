#include "MILPParameters.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>

#include <fstream>

namespace checkpoint {

namespace {

using Json = nlohmann::json;

bool readRequiredNumber(const Json &obj,
                        const char *field,
                        double &outValue,
                        std::string &errorMessage) {
    if (!obj.contains(field)) {
        errorMessage = std::string("Missing required milp parameter '") + field + "'";
        return false;
    }
    if (!obj[field].is_number()) {
        errorMessage = std::string("MILP parameter '") + field + "' must be numeric";
        return false;
    }
    outValue = obj[field].get<double>();
    return true;
}

bool readRequiredUnsigned(const Json &obj,
                          const char *field,
                          unsigned &outValue,
                          std::string &errorMessage) {
    if (!obj.contains(field)) {
        errorMessage = std::string("Missing required milp parameter '") + field + "'";
        return false;
    }
    if (!obj[field].is_number_integer() || obj[field].get<long long>() < 0) {
        errorMessage = std::string("MILP parameter '") + field +
                       "' must be a non-negative integer";
        return false;
    }
    outValue = static_cast<unsigned>(obj[field].get<unsigned long long>());
    return true;
}

bool requireNonNegative(const char *field, double value, std::string &errorMessage) {
    if (value < 0.0) {
        errorMessage = std::string("MILP parameter '") + field + "' must be >= 0";
        return false;
    }
    return true;
}

} // namespace

MILPParameterParseResult parseMILPParametersFromFile(llvm::StringRef configPath) {
    if (configPath.empty()) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::MissingConfigPath,
            "Error: -milp-config is required for milp/milp-next/milp-validate pass");
    }

    std::ifstream file(configPath.str());
    if (!file.is_open()) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::FileOpenFailed,
            ("Error: Cannot open MILP config file: " + configPath.str()));
    }

    Json root = Json::parse(file, nullptr, false);
    if (root.is_discarded()) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::InvalidJSON,
            ("Error: JSON parse error in MILP config: " + configPath.str()));
    }

    if (!root.contains("milp_parameters") || !root["milp_parameters"].is_object()) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::MissingSection,
            "Error: Missing required object 'milp_parameters'");
    }

    const Json &params = root["milp_parameters"];
    MILPParameters parsed;
    std::string errorMessage;

    if (!readRequiredNumber(params, "energy_budget_nJ", parsed.energyBudget_nJ, errorMessage) ||
        !readRequiredNumber(params, "region_prologue_overhead_nJ",
                            parsed.regionPrologueOverhead_nJ, errorMessage) ||
        !readRequiredNumber(params, "region_epilogue_overhead_nJ",
                            parsed.regionEpilogueOverhead_nJ, errorMessage) ||
        !readRequiredUnsigned(params, "vm_capacity_bytes", parsed.vmCapacityBytes, errorMessage) ||
        !readRequiredNumber(params, "save_register_to_nvm_nJ",
                            parsed.saveRegisterToNVM_nJ, errorMessage) ||
        !readRequiredNumber(params, "restore_register_from_nvm_nJ",
                            parsed.restoreRegisterFromNVM_nJ, errorMessage) ||
        !readRequiredNumber(params, "save_object_to_nvm_per_byte_nJ",
                            parsed.saveObjectToNVMPerByte_nJ, errorMessage) ||
        !readRequiredNumber(params, "restore_object_from_nvm_per_byte_nJ",
                            parsed.restoreObjectFromNVMPerByte_nJ, errorMessage) ||
        !readRequiredNumber(params, "nvm_load_penalty_per_byte_nJ",
                            parsed.nvmLoadPenaltyPerByte_nJ, errorMessage) ||
        !readRequiredNumber(params, "nvm_store_penalty_per_byte_nJ",
                            parsed.nvmStorePenaltyPerByte_nJ, errorMessage) ||
        !readRequiredNumber(params, "boundary_reboot_probability",
                            parsed.boundaryRebootProbability, errorMessage) ||
        !readRequiredUnsigned(params, "default_loop_bound", parsed.defaultLoopBound, errorMessage) ||
        !readRequiredNumber(params, "solver_time_limit_sec",
                            parsed.solverTimeLimitSec, errorMessage) ||
        !readRequiredNumber(params, "solver_mip_gap", parsed.solverMIPGap, errorMessage)) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::MissingField, errorMessage);
    }

    if (!params.contains("forbidden_boundary_blocks") ||
        !params["forbidden_boundary_blocks"].is_array()) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::MissingField,
            "Missing required milp parameter 'forbidden_boundary_blocks' (array)");
    }
    for (const Json &entry : params["forbidden_boundary_blocks"]) {
        if (!entry.is_string()) {
            return MILPParameterParseResult::error(
                MILPParameterParseResult::Status::InvalidField,
                "MILP parameter 'forbidden_boundary_blocks' must be an array of strings");
        }
        parsed.forbiddenBoundaryBlocks.push_back(entry.get<std::string>());
    }

    if (!requireNonNegative("energy_budget_nJ", parsed.energyBudget_nJ, errorMessage) ||
        !requireNonNegative("region_prologue_overhead_nJ",
                            parsed.regionPrologueOverhead_nJ, errorMessage) ||
        !requireNonNegative("region_epilogue_overhead_nJ",
                            parsed.regionEpilogueOverhead_nJ, errorMessage) ||
        !requireNonNegative("save_register_to_nvm_nJ",
                            parsed.saveRegisterToNVM_nJ, errorMessage) ||
        !requireNonNegative("restore_register_from_nvm_nJ",
                            parsed.restoreRegisterFromNVM_nJ, errorMessage) ||
        !requireNonNegative("save_object_to_nvm_per_byte_nJ",
                            parsed.saveObjectToNVMPerByte_nJ, errorMessage) ||
        !requireNonNegative("restore_object_from_nvm_per_byte_nJ",
                            parsed.restoreObjectFromNVMPerByte_nJ, errorMessage) ||
        !requireNonNegative("nvm_load_penalty_per_byte_nJ",
                            parsed.nvmLoadPenaltyPerByte_nJ, errorMessage) ||
        !requireNonNegative("nvm_store_penalty_per_byte_nJ",
                            parsed.nvmStorePenaltyPerByte_nJ, errorMessage) ||
        !requireNonNegative("solver_time_limit_sec",
                            parsed.solverTimeLimitSec, errorMessage) ||
        !requireNonNegative("solver_mip_gap", parsed.solverMIPGap, errorMessage)) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::InvalidField, errorMessage);
    }

    if (parsed.boundaryRebootProbability < 0.0 || parsed.boundaryRebootProbability > 1.0) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::InvalidField,
            "MILP parameter 'boundary_reboot_probability' must be within [0, 1]");
    }
    if (parsed.defaultLoopBound == 0) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::InvalidField,
            "MILP parameter 'default_loop_bound' must be > 0");
    }
    if (parsed.vmCapacityBytes == 0) {
        return MILPParameterParseResult::error(
            MILPParameterParseResult::Status::InvalidField,
            "MILP parameter 'vm_capacity_bytes' must be > 0");
    }

    return MILPParameterParseResult::ok(std::move(parsed));
}

} // namespace checkpoint
