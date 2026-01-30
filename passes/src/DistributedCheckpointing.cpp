#include "DistributedCheckpointing.h"
#include "BlockUtils.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <algorithm>
#include <cstdint>

namespace checkpoint {

DistributedCheckpointing::DistributedCheckpointing(
    llvm::Function &F,
    const std::vector<RegionInfo> &regions,
    const std::vector<std::string> &boundaries)
    : F_(F), regions_(regions), boundaries_(boundaries) {
    buildBlockMap();
}

void DistributedCheckpointing::buildBlockMap() {
    blockMap_.clear();
    for (llvm::BasicBlock &BB : F_) {
        std::string name = getBlockName(BB, F_);
        blockMap_[name] = &BB;
    }
}

std::set<llvm::Value*> DistributedCheckpointing::computeDefs(
    const RegionInfo &region) {
    std::set<llvm::Value*> defs;

    for (const auto &blockName : region.blocks) {
        auto it = blockMap_.find(blockName);
        if (it == blockMap_.end()) continue;

        llvm::BasicBlock *BB = it->second;
        for (llvm::Instruction &I : *BB) {
            // Skip void-returning instructions (no definition)
            if (I.getType()->isVoidTy()) continue;

            // Skip PHI nodes - they don't represent new definitions
            // in the same sense (they're merge points)
            if (llvm::isa<llvm::PHINode>(&I)) continue;

            // Skip allocas - they're stack slots, not registers
            if (llvm::isa<llvm::AllocaInst>(&I)) continue;

            // This instruction defines a value (register in SSA form)
            defs.insert(&I);
        }
    }

    return defs;
}

std::set<llvm::Value*> DistributedCheckpointing::computeLiveOut(
    const RegionInfo &region) {
    std::set<llvm::Value*> liveOut;

    // A value is live-out of a region if:
    // 1. It's defined in the region, AND
    // 2. It has uses outside the region

    // First, collect all blocks in the region
    std::set<std::string> regionBlockSet(region.blocks.begin(),
                                          region.blocks.end());

    // For each definition in the region, check if any use is outside
    std::set<llvm::Value*> defs = computeDefs(region);

    for (llvm::Value *def : defs) {
        for (llvm::User *user : def->users()) {
            if (auto *userInst = llvm::dyn_cast<llvm::Instruction>(user)) {
                llvm::BasicBlock *userBB = userInst->getParent();
                std::string userBlockName = getBlockName(*userBB, F_);

                // If the use is in a block outside this region, value is live-out
                if (!regionBlockSet.count(userBlockName)) {
                    liveOut.insert(def);
                    break;  // No need to check other uses
                }
            }
        }
    }

    return liveOut;
}

llvm::Instruction* DistributedCheckpointing::findLastDef(
    llvm::Value *reg,
    const RegionInfo &region) {
    // The register IS the instruction that defines it in LLVM IR
    // So we just need to verify it's in this region and return it
    auto *inst = llvm::dyn_cast<llvm::Instruction>(reg);
    if (!inst) return nullptr;

    llvm::BasicBlock *BB = inst->getParent();
    std::string blockName = getBlockName(*BB, F_);

    // Verify it's in this region
    for (const auto &b : region.blocks) {
        if (b == blockName) {
            return inst;
        }
    }

    return nullptr;
}

unsigned DistributedCheckpointing::assignRegId(llvm::Value *reg) {
    auto it = regIdMap_.find(reg);
    if (it != regIdMap_.end()) {
        return it->second;
    }
    unsigned id = nextRegId_++;
    regIdMap_[reg] = id;
    return id;
}

std::vector<CheckpointPoint> DistributedCheckpointing::analyze() {
    std::vector<CheckpointPoint> checkpoints;

    for (size_t i = 0; i < regions_.size(); ++i) {
        const RegionInfo &region = regions_[i];

        // Compute Def_r ∩ LiveOut_r
        std::set<llvm::Value*> defs = computeDefs(region);
        std::set<llvm::Value*> liveOut = computeLiveOut(region);

        // Intersection: registers to checkpoint
        std::set<llvm::Value*> toCheckpoint;
        for (llvm::Value *v : defs) {
            if (liveOut.count(v)) {
                toCheckpoint.insert(v);
            }
        }

        // For each register to checkpoint, find its definition point
        for (llvm::Value *reg : toCheckpoint) {
            llvm::Instruction *defInst = findLastDef(reg, region);
            if (defInst) {
                CheckpointPoint ckpt;
                ckpt.afterInst = defInst;
                ckpt.reg = reg;
                ckpt.regId = assignRegId(reg);
                ckpt.regionName = region.startBlock;
                checkpoints.push_back(ckpt);
            }
        }
    }

    return checkpoints;
}

// === Memory checkpointing analysis implementation ===

bool DistributedCheckpointing::isModifiedInRegion(
    llvm::AllocaInst *alloca,
    const RegionInfo &region) {

    std::set<std::string> regionBlockSet(region.blocks.begin(),
                                          region.blocks.end());

    // Check if any store instruction to this alloca is in the region
    for (llvm::User *user : alloca->users()) {
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
            // Check if storing TO this alloca (not FROM)
            if (store->getPointerOperand() == alloca) {
                llvm::BasicBlock *storeBB = store->getParent();
                std::string storeName = getBlockName(*storeBB, F_);
                if (regionBlockSet.count(storeName)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool DistributedCheckpointing::isModifiedInRegion(
    llvm::GlobalVariable *global,
    const RegionInfo &region) {

    std::set<std::string> regionBlockSet(region.blocks.begin(),
                                          region.blocks.end());

    // Check if any store instruction to this global is in the region
    for (llvm::User *user : global->users()) {
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
            // Check if storing TO this global (not FROM)
            if (store->getPointerOperand() == global) {
                llvm::BasicBlock *storeBB = store->getParent();
                std::string storeName = getBlockName(*storeBB, F_);
                if (regionBlockSet.count(storeName)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool DistributedCheckpointing::isReadAfterRegion(
    llvm::Value *memLoc,
    const RegionInfo &region) {

    std::set<std::string> regionBlockSet(region.blocks.begin(),
                                          region.blocks.end());

    // Check if any load instruction from this memory location is outside the region
    for (llvm::User *user : memLoc->users()) {
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
            llvm::BasicBlock *loadBB = load->getParent();
            std::string loadName = getBlockName(*loadBB, F_);

            // If load is outside this region, memory is live-out
            if (!regionBlockSet.count(loadName)) {
                return true;
            }
        }
    }
    return false;
}

std::set<llvm::AllocaInst*> DistributedCheckpointing::computeLiveAllocas(
    const RegionInfo &region) {

    std::set<llvm::AllocaInst*> liveAllocas;

    // Find all allocas in this function
    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                // Alloca is live-out if:
                // 1. Modified (stored to) in this region, AND
                // 2. Read (loaded from) after this region
                if (isModifiedInRegion(alloca, region) &&
                    isReadAfterRegion(alloca, region)) {
                    liveAllocas.insert(alloca);
                }
            }
        }
    }

    return liveAllocas;
}

std::set<llvm::GlobalVariable*> DistributedCheckpointing::computeLiveGlobals(
    const RegionInfo &region) {

    std::set<llvm::GlobalVariable*> liveGlobals;
    llvm::Module *M = F_.getParent();

    // Check each global variable in the module
    for (llvm::GlobalVariable &GV : M->globals()) {
        // Skip constants and internal globals that aren't user-defined
        if (GV.isConstant()) continue;
        if (GV.getName().starts_with("llvm.")) continue;
        if (GV.getName().starts_with("__nvm_")) continue;  // Skip our NVM globals

        // Global is live-out if:
        // 1. Modified (stored to) in this region, AND
        // 2. Read (loaded from) after this region
        if (isModifiedInRegion(&GV, region) &&
            isReadAfterRegion(&GV, region)) {
            liveGlobals.insert(&GV);
        }
    }

    return liveGlobals;
}

std::string DistributedCheckpointing::generateNVMSlotName(
    unsigned boundaryId,
    llvm::Value *memLoc) {

    std::string baseName;

    if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(memLoc)) {
        baseName = alloca->getName().str();
        if (baseName.empty()) {
            // Generate name based on instruction address
            baseName = "local_" + std::to_string(
                reinterpret_cast<uintptr_t>(alloca) & 0xFFFF);
        }
    } else if (auto *global = llvm::dyn_cast<llvm::GlobalVariable>(memLoc)) {
        baseName = global->getName().str();
    } else {
        baseName = "mem";
    }

    return "__nvm_b" + std::to_string(boundaryId) + "_" + baseName;
}

MemoryCheckpointResult DistributedCheckpointing::analyzeMemory() {
    MemoryCheckpointResult result;

    // We checkpoint memory at region boundaries.
    // Each region (except the first) starts with a boundary.
    // Region i's boundary is at boundaries_[i-1] (shifted by 1 since
    // entry doesn't have a preceding boundary).

    for (size_t i = 0; i < regions_.size(); ++i) {
        const RegionInfo &region = regions_[i];

        // Skip first region (entry) - it has no preceding boundary
        // We checkpoint at the END of region i, which is the boundary of region i+1
        // So we associate checkpoints with boundary i (which starts region i+1)

        // Find allocas and globals live at this region's exit
        std::set<llvm::AllocaInst*> liveAllocas = computeLiveAllocas(region);
        std::set<llvm::GlobalVariable*> liveGlobals = computeLiveGlobals(region);

        // Use the region index as the boundary ID
        // (boundary i is at the start of region i+1, but we save at end of region i)
        unsigned boundaryId = static_cast<unsigned>(i);

        // Create checkpoint points for allocas
        for (llvm::AllocaInst *alloca : liveAllocas) {
            MemoryCheckpointPoint ckpt;
            ckpt.memLoc = alloca;
            ckpt.valueType = alloca->getAllocatedType();
            ckpt.nvmSlotName = generateNVMSlotName(boundaryId, alloca);
            ckpt.boundaryId = boundaryId;
            ckpt.regionName = region.startBlock;

            result.checkpoints.push_back(ckpt);
            result.byBoundary[boundaryId].push_back(ckpt);
        }

        // Create checkpoint points for globals
        for (llvm::GlobalVariable *global : liveGlobals) {
            MemoryCheckpointPoint ckpt;
            ckpt.memLoc = global;
            ckpt.valueType = global->getValueType();
            ckpt.nvmSlotName = generateNVMSlotName(boundaryId, global);
            ckpt.boundaryId = boundaryId;
            ckpt.regionName = region.startBlock;

            result.checkpoints.push_back(ckpt);
            result.byBoundary[boundaryId].push_back(ckpt);
        }
    }

    return result;
}

} // namespace checkpoint
