#include "MILPModelTypes.h"

#include "llvm/IR/Instructions.h"

namespace checkpoint {

DefinitionSiteInventory collectDefinitionSiteInventory(llvm::Function &F) {
    DefinitionSiteInventory inventory;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            if (!I.getType()->isVoidTy() && !llvm::isa<llvm::AllocaInst>(&I)) {
                ++inventory.ssaDefinitionSites;
            }

            if (llvm::isa<llvm::StoreInst>(&I)) {
                ++inventory.storeDefinitionSites;
            }

            if (auto *call = llvm::dyn_cast<llvm::CallBase>(&I)) {
                if (call->mayWriteToMemory()) {
                    ++inventory.memoryWritingCallDefinitionSites;
                }
            }
        }
    }

    return inventory;
}

MILPVariableInventory buildMILPVariableInventory(
    size_t basicBlockCount,
    size_t definitionSiteCount,
    size_t vmCandidateObjectCount,
    size_t estimatedLiveInObjectPairCount) {

    MILPVariableInventory inventory;
    inventory.isRegionStartCount = basicBlockCount;
    inventory.isCheckpointEnabledCount = definitionSiteCount;
    inventory.isObjectInVMCount = vmCandidateObjectCount;
    inventory.needsVMObjectRestoreCount = estimatedLiveInObjectPairCount;
    inventory.accumulatedRegionEnergyCount = basicBlockCount;
    return inventory;
}

} // namespace checkpoint
