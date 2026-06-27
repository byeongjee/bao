#pragma once

#include "schematic/SchematicBlock.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Value.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace checkpoint {

enum class Placement { VM, NVM };

enum class CheckpointState {
    Potential,
    Disabled,
    Active,
    Virtual, // call_entry -> call_exit edge when the callee contains a checkpoint
    LoopLatch,
};

struct CFGEdge {
    SchematicBlock *src;
    SchematicBlock *dst;

    bool operator<(const CFGEdge &o) const {
        if (src != o.src)
            return src < o.src;
        return dst < o.dst;
    }

    bool operator==(const CFGEdge &o) const { return src == o.src && dst == o.dst; }
};

/// Resolve a checkpoint edge to a real CFG edge.
/// Function-level traces have loops collapsed, so RCG-selected checkpoints
/// may reference edges (src->dst) where dst is not a direct CFG successor of
/// src (the loop body is between them).  The reference implementation uses a
/// loop-collapsed CFG where these edges exist; we map them to the actual
/// LLVM CFG edge: src -> successor_of_src that can reach dst via BFS.
/// Returns the resolved edge, or the original if already valid.
inline CFGEdge resolveCheckpointEdge(const CFGEdge &edge) {
    for (SchematicBlock *succ : edge.src->successors()) {
        if (succ == edge.dst)
            return edge;
    }
    // BFS from each successor to find which one reaches dst.
    for (SchematicBlock *succ : edge.src->successors()) {
        std::set<SchematicBlock *> visited;
        std::vector<SchematicBlock *> worklist = {succ};
        while (!worklist.empty()) {
            SchematicBlock *cur = worklist.back();
            worklist.pop_back();
            if (cur == edge.dst)
                return CFGEdge{edge.src, succ};
            if (!visited.insert(cur).second)
                continue;
            for (SchematicBlock *s : cur->successors())
                worklist.push_back(s);
        }
    }
    return edge; // fallback: return original
}

/// Per-variable allocation info within a region.
/// Reference: memory_allocation.py:54-105 (VariableAllocation class).
struct VariableAllocation {
    Placement placement = Placement::NVM;
    bool rawNeedRestore = true;
    bool rawNeedSave = true;

    /// Reference: memory_allocation.py:100-101
    bool needRestore() const { return rawNeedRestore && placement == Placement::VM; }
    /// Reference: memory_allocation.py:104-105
    bool needSave() const { return rawNeedSave && placement == Placement::VM; }
};

struct RegionAllocation {
    std::map<llvm::Value *, VariableAllocation> vars;
    std::map<llvm::Value *, unsigned> vmOffsets; // byte offset in VM
    unsigned vmBytesUsed = 0;
    double intervalEnergy = 0.0;
};

/// Matches Python's bb.loop = Loop(loop_info, nb_iter, cost_one_it).
/// Set after loop analysis; used by propagateEnergy() for multi-iteration scaling.
struct LoopMark {
    llvm::Loop *loop;
    unsigned nbIter;  // iterations between checkpoints
    double costOneIt; // energy cost of one iteration
};

/// Per-block metadata for multi-path overlap (spec S8.3)
struct BlockMetadata {
    bool analyzed = false;
    double E_left = std::numeric_limits<double>::max(); // energy remaining after block
    double E_to_leave = 0.0;                            // min energy needed at entry
    std::optional<LoopMark> loop; // set after loop analysis for multi-iteration scaling
};

struct LoopCheckpointDecision {
    llvm::Loop *loop = nullptr;
    bool mandatoryBackEdge = false;      // alloc(H) != alloc(L)
    bool loopFitsEntirely = false;       // entire loop fits without checkpoint
    unsigned numIterationsPerCharge = 0; // conditional checkpoint interval
    double E_loop = 0.0;                 // per-iteration energy
    unsigned bodyPathCount = 0;
    bool hadEnabledCheckpoints = false;
    bool convergenceApplied = false;
    unsigned convergenceIterations = 0;
    double initialStartEToLeave = 0.0;
    double initialEndEToLeave = 0.0;
    double initialELoop = 0.0;
    double initialAvailableEnergy = 0.0;
    int initialRawNumIterations = 0;
    double finalStartEToLeave = 0.0;
    double finalEndEToLeave = 0.0;
    double finalAvailableEnergy = 0.0;
    int finalRawNumIterations = 0;
    RegionAllocation bodyAllocation;
};

struct RegionSolution {
    std::vector<SchematicBlock *> blocks;
    RegionAllocation allocation;
};

struct SchematicSolution {
    std::set<CFGEdge> enabledCheckpoints;
    std::map<CFGEdge, CheckpointState> checkpointStates;
    std::map<CFGEdge, std::vector<std::string>> checkpointOrigins;
    std::vector<RegionSolution> regions;
    std::unordered_map<SchematicBlock *, BlockMetadata> blockMeta;
    std::unordered_map<SchematicBlock *, LoopCheckpointDecision> loopDecisions;
    /// Per-block decided variable placements from earlier analyses.
    /// Enforces allocation consistency across paths (spec S12.2).
    std::unordered_map<SchematicBlock *, std::map<llvm::Value *, Placement>> decidedPlacements;
    /// Per-block memory allocation, matching Python's bb.memory_allocation.
    /// Multiple blocks in the same region share the same allocation object.
    /// Reference: memory_allocation.py MemoryAllocation class.
    std::unordered_map<SchematicBlock *, std::shared_ptr<RegionAllocation>> blockAllocation;
    /// call_entry / call_exit blocks created by call isolation. The RCG skips the
    /// call-interior edge (call_entry -> call_exit). Ref: bb.is_function_call.
    std::set<SchematicBlock *> functionCallBlocks;
    unsigned pathsAnalyzed = 0;
    unsigned totalVmVariables = 0;
    unsigned totalNvmVariables = 0;
};

inline CFGEdge getResolvedCheckpointEdge(const CFGEdge &edge) {
    return resolveCheckpointEdge(edge);
}

inline CheckpointState getCheckpointState(const SchematicSolution &solution, const CFGEdge &edge) {
    CFGEdge resolved = getResolvedCheckpointEdge(edge);
    auto it = solution.checkpointStates.find(resolved);
    if (it == solution.checkpointStates.end())
        return CheckpointState::Potential;
    return it->second;
}

inline bool isDisabledCheckpoint(const SchematicSolution &solution, const CFGEdge &edge) {
    return getCheckpointState(solution, edge) == CheckpointState::Disabled;
}

inline bool isPotentialCheckpoint(const SchematicSolution &solution, const CFGEdge &edge) {
    return getCheckpointState(solution, edge) == CheckpointState::Potential;
}

inline void setCheckpointState(SchematicSolution &solution, const CFGEdge &edge,
                               CheckpointState state, const std::string &origin = "") {
    CFGEdge resolved = resolveCheckpointEdge(edge);
    solution.checkpointStates[resolved] = state;

    if (state == CheckpointState::Active) {
        solution.enabledCheckpoints.insert(resolved);
        if (!origin.empty()) {
            auto &origins = solution.checkpointOrigins[resolved];
            if (std::find(origins.begin(), origins.end(), origin) == origins.end())
                origins.push_back(origin);
        }
        return;
    }

    solution.enabledCheckpoints.erase(resolved);
    if (state != CheckpointState::Active)
        solution.checkpointOrigins.erase(resolved);
}

inline void enableCheckpoint(SchematicSolution &solution, const CFGEdge &edge,
                             const std::string &origin) {
    setCheckpointState(solution, edge, CheckpointState::Active, origin);
}

inline void disableCheckpoint(SchematicSolution &solution, const CFGEdge &edge) {
    setCheckpointState(solution, edge, CheckpointState::Disabled);
}

inline void setLoopLatchCheckpoint(SchematicSolution &solution, const CFGEdge &edge) {
    setCheckpointState(solution, edge, CheckpointState::LoopLatch);
}

/// True if the solved function contains any non-DISABLED checkpoint, i.e. the
/// inter-procedural fold must treat a call to it as a VIRTUAL (wall) boundary
/// rather than a transparent DISABLED fold. Faithful to the reference's
/// `not has_only_disabled_checkpoints()` (schematic.py:688). Active/LoopLatch/
/// Virtual checkpoint states, an enabled checkpoint, or a loop that takes a
/// back-edge checkpoint / charges per-iteration all count as "has checkpoint".
inline bool hasNonDisabledCheckpoint(const SchematicSolution &solution) {
    if (!solution.enabledCheckpoints.empty())
        return true;
    for (const auto &[edge, st] : solution.checkpointStates) {
        (void)edge;
        if (st == CheckpointState::Active || st == CheckpointState::LoopLatch ||
            st == CheckpointState::Virtual)
            return true;
    }
    for (const auto &[block, dec] : solution.loopDecisions) {
        (void)block;
        // A loop that fits entirely in one charge takes NO checkpoint (the latch
        // is DISABLED), so it must not count — even though numIterationsPerCharge
        // is set to the trip count. Genuine loop checkpoints (alloc-mismatch =>
        // Active, conditional => LoopLatch, nested-call wall => Virtual) are
        // already captured by the checkpointStates loop above. Ref: a fits-entirely
        // loop sets chkpt.type = DISABLED (schematic.py:620-622).
        if (dec.loopFitsEntirely)
            continue;
        if (dec.mandatoryBackEdge || dec.numIterationsPerCharge > 0 || dec.hadEnabledCheckpoints)
            return true;
    }
    return false;
}

} // namespace checkpoint
