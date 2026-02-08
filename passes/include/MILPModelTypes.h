#pragma once

#include "llvm/IR/Function.h"

#include <cstddef>

namespace checkpoint {

/// Inventory of definition sites discovered before MILP construction.
struct DefinitionSiteInventory {
    size_t ssaDefinitionSites = 0;
    size_t storeDefinitionSites = 0;
    size_t memoryWritingCallDefinitionSites = 0;

    size_t totalDefinitionSites() const {
        return ssaDefinitionSites + storeDefinitionSites + memoryWritingCallDefinitionSites;
    }
};

/// Size of MILP variable groups using descriptive names.
struct MILPVariableInventory {
    size_t isRegionStartCount = 0;
    size_t isCheckpointEnabledCount = 0;
    size_t isObjectInVMCount = 0;
    size_t needsVMObjectRestoreCount = 0;
    size_t accumulatedRegionEnergyCount = 0;
};

DefinitionSiteInventory collectDefinitionSiteInventory(llvm::Function &F);

MILPVariableInventory buildMILPVariableInventory(
    size_t basicBlockCount,
    size_t definitionSiteCount,
    size_t vmCandidateObjectCount,
    size_t estimatedLiveInObjectPairCount);

} // namespace checkpoint
