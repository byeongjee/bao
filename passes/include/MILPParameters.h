#pragma once

#include "llvm/ADT/StringRef.h"

#include <string>
#include <utility>
#include <vector>

namespace checkpoint {

/// Required parameters for MILP optimization and validation.
/// All energy values are expressed in nanojoules (nJ).
struct MILPParameters {
    double energyBudget_nJ = 0.0;
    double regionPrologueOverhead_nJ = 0.0;
    double regionEpilogueOverhead_nJ = 0.0;

    unsigned vmCapacityBytes = 0;

    double saveRegisterToNVM_nJ = 0.0;
    double restoreRegisterFromNVM_nJ = 0.0;
    double saveObjectToNVMPerByte_nJ = 0.0;
    double restoreObjectFromNVMPerByte_nJ = 0.0;
    double nvmLoadPenaltyPerByte_nJ = 0.0;
    double nvmStorePenaltyPerByte_nJ = 0.0;

    double boundaryRebootProbability = 0.0;
    unsigned defaultLoopBound = 0;

    double solverTimeLimitSec = 0.0;
    double solverMIPGap = 0.0;

    std::vector<std::string> forbiddenBoundaryBlocks;
};

struct MILPParameterParseResult {
    enum class Status {
        Success,
        MissingConfigPath,
        FileOpenFailed,
        InvalidJSON,
        MissingSection,
        MissingField,
        InvalidField
    };

    Status status = Status::Success;
    MILPParameters parameters;
    std::string errorMessage;

    bool success() const { return status == Status::Success; }

    static MILPParameterParseResult ok(MILPParameters params) {
        MILPParameterParseResult result;
        result.status = Status::Success;
        result.parameters = std::move(params);
        return result;
    }

    static MILPParameterParseResult error(Status st, std::string message) {
        MILPParameterParseResult result;
        result.status = st;
        result.errorMessage = std::move(message);
        return result;
    }
};

/// Parse required MILP parameters from JSON file.
MILPParameterParseResult parseMILPParametersFromFile(llvm::StringRef configPath);

} // namespace checkpoint
