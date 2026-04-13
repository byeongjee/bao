#pragma once

#include "llvm/ADT/StringRef.h"

namespace checkpoint {

struct RockClimbParams {
    double capacity = 0.0;
    double EPro = 0.0;
    double EEpi = 0.0;
    unsigned NReg = 0;
    double regRestoreEnergy = 0.0;
    double regStoreEnergy = 0.0;
    bool distributedCheckpointing = true;
    bool addDebugMarkers = false;

    double calculateESafe() const {
        // PC and SP recovery are accounted for in E_pro already.
        return capacity - EPro - EEpi - (NReg - 2) * regRestoreEnergy;
    }
};

bool parseRockClimbParams(llvm::StringRef configPath, RockClimbParams &params);

} // namespace checkpoint
