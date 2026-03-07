#include "rockclimb/DistributedCheckpointing.h"
#include "common/BlockUtils.h"

#include "llvm/IR/Instructions.h"
#include <algorithm>

namespace checkpoint {

DistributedCheckpointing::DistributedCheckpointing(const std::vector<RegionInfo> &regions)
    : regions_(regions) {}

llvm::SmallPtrSet<llvm::BasicBlock *, 8>
DistributedCheckpointing::makeRegionBlockSet(const RegionInfo &region) {
    llvm::SmallPtrSet<llvm::BasicBlock *, 8> blockSet;
    for (const auto &handle : region.blocks) {
        llvm::BasicBlock *BB = RockClimbOptimizer::resolveBlock(handle);
        blockSet.insert(BB);
    }
    return blockSet;
}

std::set<llvm::Value *> DistributedCheckpointing::computeDefs(const RegionInfo &region) {
    std::set<llvm::Value *> defs;

    for (const auto &handle : region.blocks) {
        llvm::BasicBlock *BB = RockClimbOptimizer::resolveBlock(handle);
        for (llvm::Instruction &I : *BB) {
            // Skip void-returning instructions (no definition)
            if (I.getType()->isVoidTy())
                continue;

            // Skip allocas - they're stack slots, not registers
            if (llvm::isa<llvm::AllocaInst>(&I))
                continue;

            // This instruction defines a value (register in SSA form)
            defs.insert(&I);
        }
    }

    return defs;
}

std::set<llvm::Value *> DistributedCheckpointing::computeLiveOut(const RegionInfo &region) {
    std::set<llvm::Value *> liveOut;

    // A value is live-out of a region if:
    // 1. It's defined in the region, AND
    // 2. It has uses outside the region

    // First, collect all blocks in the region
    auto regionBlockSet = makeRegionBlockSet(region);

    // For each definition in the region, check if any use is outside
    std::set<llvm::Value *> defs = computeDefs(region);

    for (llvm::Value *def : defs) {
        for (llvm::User *user : def->users()) {
            if (auto *userInst = llvm::dyn_cast<llvm::Instruction>(user)) {
                llvm::BasicBlock *userBB = userInst->getParent();

                // If the use is in a block outside this region, value is live-out
                if (!regionBlockSet.count(userBB)) {
                    liveOut.insert(def);
                    break; // No need to check other uses
                }
            }
        }
    }

    return liveOut;
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
        std::set<llvm::Value *> defs = computeDefs(region);
        std::set<llvm::Value *> liveOut = computeLiveOut(region);

        // Intersection: registers to checkpoint
        std::set<llvm::Value *> toCheckpoint;
        for (llvm::Value *v : defs) {
            if (liveOut.count(v)) {
                toCheckpoint.insert(v);
            }
        }

        // For each register to checkpoint, find its definition point
        auto regionBlockSet = makeRegionBlockSet(region);
        for (llvm::Value *reg : toCheckpoint) {
            auto *inst = llvm::dyn_cast<llvm::Instruction>(reg);
            if (inst && regionBlockSet.count(inst->getParent())) {
                CheckpointPoint ckpt;
                ckpt.afterInst = inst;
                ckpt.reg = reg;
                ckpt.regId = assignRegId(reg);
                ckpt.regionStart = RockClimbOptimizer::resolveBlock(region.startBlock);
                checkpoints.push_back(ckpt);
            }
        }
    }

    return checkpoints;
}

} // namespace checkpoint
