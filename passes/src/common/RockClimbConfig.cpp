#include "common/RockClimbConfig.h"

#include "common/Logger.h"

#include "llvm/Support/JSON.h"

#include <fstream>
#include <optional>
#include <sstream>

namespace checkpoint {

bool parseRockClimbParams(llvm::StringRef configPath, RockClimbParams &params) {
    std::ifstream file(configPath.str());
    if (!file.is_open()) {
        PLOGE << "Error: Cannot open RockClimb config: " << configPath;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(buffer.str());
    if (!parsed) {
        llvm::consumeError(parsed.takeError());
        PLOGE << "Error: JSON parse error in RockClimb config: " << configPath;
        return false;
    }

    llvm::json::Object *root = parsed->getAsObject();
    if (!root) {
        PLOGE << "Error: RockClimb config is not a JSON object: " << configPath;
        return false;
    }

    auto capacity = root->getNumber("capacity");
    if (!capacity) {
        auto legacy = root->getNumber("E_input");
        if (!legacy) {
            PLOGE << "Error: Missing 'capacity' in RockClimb config";
            return false;
        }
        capacity = legacy;
    }

    auto NReg = root->getInteger("N_reg");
    if (!NReg) {
        PLOGE << "Error: Missing 'N_reg' in RockClimb config";
        return false;
    }

    auto regStoreEnergy = root->getNumber("reg_store_energy");
    if (!regStoreEnergy) {
        PLOGE << "Error: Missing 'reg_store_energy' in RockClimb config";
        return false;
    }

    auto regRestoreEnergy = root->getNumber("reg_restore_energy");
    if (!regRestoreEnergy) {
        PLOGE << "Error: Missing 'reg_restore_energy' in RockClimb config";
        return false;
    }

    auto EPro = root->getNumber("E_pro");
    if (!EPro) {
        PLOGE << "Error: Missing 'E_pro' in RockClimb config";
        return false;
    }

    auto EEpi = root->getNumber("E_epi");
    if (!EEpi) {
        PLOGE << "Error: Missing 'E_epi' in RockClimb config";
        return false;
    }

    if (*NReg < 2) {
        PLOGE << "Error: N_reg must be >= 2 (must include at least PC and SP)";
        return false;
    }

    params.capacity = *capacity;
    params.EPro = *EPro;
    params.EEpi = *EEpi;
    params.NReg = static_cast<unsigned>(*NReg);
    params.regStoreEnergy = *regStoreEnergy;
    params.regRestoreEnergy = *regRestoreEnergy;

    llvm::json::Object *rcSection = nullptr;
    if (auto *rcVal = root->get("rockclimb"))
        rcSection = rcVal->getAsObject();

    std::optional<bool> distributed;
    if (rcSection) {
        if (auto val = rcSection->getBoolean("distributed_checkpointing"))
            distributed = val;
    }
    if (!distributed) {
        if (auto val = root->getBoolean("distributed_checkpointing"))
            distributed = val;
    }
    params.distributedCheckpointing = distributed.value_or(true);

    if (auto val = root->getBoolean("add_debug_markers"))
        params.addDebugMarkers = *val;

    return true;
}

} // namespace checkpoint
