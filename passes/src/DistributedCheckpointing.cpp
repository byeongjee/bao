#include "DistributedCheckpointing.h"
#include "BlockUtils.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>

namespace checkpoint {

DistributedCheckpointing::DistributedCheckpointing(
    llvm::Function &F,
    const std::vector<RegionInfo> &regions)
    : F_(F), regions_(regions) {
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

} // namespace checkpoint
