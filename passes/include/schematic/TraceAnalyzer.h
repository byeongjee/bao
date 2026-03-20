#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include <vector>

namespace checkpoint {

/// Segment extraction: split a trace into contiguous unanalyzed sub-paths.
/// Returns segments + their start/end boundary blocks.
/// Reference: schematic.py:extract_not_fixed_bb_paths (line 315).
struct ExtractedSegments {
    std::vector<std::vector<llvm::BasicBlock *>> segments;
    std::vector<llvm::BasicBlock *> startBoundaries;
    std::vector<llvm::BasicBlock *> endBoundaries;
};

ExtractedSegments extractNotFixedBBPaths(const std::vector<llvm::BasicBlock *> &trace,
                                         const SchematicSolution &solution);

/// Analyze a single trace: extract segments, solve RCG for each, apply allocations.
/// Reference: schematic.py:analyze_trace (line 351).
/// Returns true if all segments are feasible. On failure, errorMessage is set.
bool analyzeTrace(const std::vector<llvm::BasicBlock *> &trace, SchematicSolution &solution,
                  const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                  const SchematicParams &params, VMAddressTracker *tracker, llvm::LoopInfo &LI,
                  llvm::Loop *loopScope, std::string &errorMessage);

/// Build a synthetic trace starting from an unanalyzed block.
/// Reference: schematic.py:extract_not_fixed_bb_trace (in find_and_analyse_not_fixed_paths).
struct ExtractedTrace {
    std::vector<llvm::BasicBlock *> blocks;
    llvm::BasicBlock *startBoundary;
    llvm::BasicBlock *endBoundary;
};

ExtractedTrace extractNotFixedBBTrace(llvm::BasicBlock *startBB, const SchematicSolution &solution);

/// Analyze all uncovered blocks by building synthetic traces and running analyzeTrace.
/// Reference: schematic.py:find_and_analyse_not_fixed_paths (line 504).
/// Returns true if all traces are feasible. On failure, errorMessage is set.
bool findAndAnalyzeNotFixedPaths(const CFGAnalysis &cfg, SchematicSolution &solution,
                                 const SchematicStateAnalysis &state, const SchematicParams &params,
                                 VMAddressTracker *tracker, llvm::LoopInfo &LI,
                                 llvm::Loop *loopScope, std::string &errorMessage);

/// Single pass over CFG edges: enable checkpoints where allocations differ
/// or energy is insufficient, propagate energy otherwise.
/// When loopScope is non-null, only processes edges within that loop.
/// Reference: schematic.py:remove_potential_checkpoints_between_fixed_bbs (line 468).
void removePotentialCheckpointsBetweenFixedBBs(const CFGAnalysis &cfg, SchematicSolution &solution,
                                               const SchematicStateAnalysis &state,
                                               const SchematicParams &params, llvm::LoopInfo &LI,
                                               llvm::Loop *loopScope = nullptr);

} // namespace checkpoint
