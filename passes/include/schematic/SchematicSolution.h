#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"

#include <limits>
#include <map>
#include <set>
#include <vector>

namespace checkpoint {

enum class Placement { VM, NVM };

struct CFGEdge {
    llvm::BasicBlock *src;
    llvm::BasicBlock *dst;

    bool operator<(const CFGEdge &o) const {
        if (src != o.src)
            return src < o.src;
        return dst < o.dst;
    }

    bool operator==(const CFGEdge &o) const { return src == o.src && dst == o.dst; }
};

struct RegionAllocation {
    std::map<llvm::Value *, Placement> placement;
    std::map<llvm::Value *, unsigned> vmOffsets; // byte offset in VM
    /// Save/restore flags per variable: (needRestore, needSave).
    /// needRestore=true means restore needed at interval start (first access is a load).
    /// needSave=true means save needed at interval end (always true in reference algorithm).
    std::map<llvm::Value *, std::pair<bool, bool>> livenessFlags;
    unsigned vmBytesUsed = 0;
    double intervalEnergy = 0.0;
};

/// Per-block metadata for multi-path overlap (spec §8.3)
struct BlockMetadata {
    bool analyzed = false;
    double E_left = std::numeric_limits<double>::max(); // energy remaining after block
    double E_to_leave = 0.0;                            // min energy needed at entry
};

struct LoopCheckpointDecision {
    llvm::Loop *loop = nullptr;
    bool mandatoryBackEdge = false;      // alloc(H) != alloc(L)
    bool loopFitsEntirely = false;       // entire loop fits without checkpoint
    unsigned numIterationsPerCharge = 0; // conditional checkpoint interval
    double E_loop = 0.0;                 // per-iteration energy
    RegionAllocation bodyAllocation;
};

struct RegionSolution {
    std::vector<llvm::BasicBlock *> blocks;
    RegionAllocation allocation;
};

struct SchematicSolution {
    std::set<CFGEdge> enabledCheckpoints;
    std::vector<RegionSolution> regions;
    llvm::DenseMap<llvm::BasicBlock *, BlockMetadata> blockMeta;
    llvm::DenseMap<llvm::BasicBlock *, LoopCheckpointDecision> loopDecisions;
    /// Per-block decided variable placements from earlier analyses.
    /// Enforces allocation consistency across paths (spec §12.2).
    llvm::DenseMap<llvm::BasicBlock *, std::map<llvm::Value *, Placement>> decidedPlacements;
    unsigned pathsAnalyzed = 0;
    unsigned totalVmVariables = 0;
    unsigned totalNvmVariables = 0;
};

} // namespace checkpoint
