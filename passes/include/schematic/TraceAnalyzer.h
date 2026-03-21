#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/SchematicBlock.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include <vector>

namespace checkpoint {

/// Extract the subpaths of contiguous basic blocks not fixed following a trace.
/// Reference: schematic.py:extract_not_fixed_bb_paths (line 315).
std::vector<std::vector<SchematicBlock *>>
extractNotFixedBBPaths(const std::vector<SchematicBlock *> &trace,
                       const SchematicSolution &solution);

/// Analyze a single trace: extract segments, solve RCG for each, apply allocations.
/// Reference: schematic.py:analyze_trace (line 351).
/// Returns true if all segments are feasible. On failure, errorMessage is set.
bool analyzeTrace(const std::vector<SchematicBlock *> &trace, SchematicSolution &solution,
                  const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                  const SchematicParams &params, VMAddressTracker *tracker, llvm::LoopInfo &LI,
                  llvm::Loop *loopScope, std::string &errorMessage);

/// Extract a path of not fixed basic blocks from a given bb not fixed.
/// Returns a trace with fixed predecessor prepended and fixed successor appended.
/// Reference: schematic.py:extract_not_fixed_bb_trace (line 295).
std::vector<SchematicBlock *> extractNotFixedBBTrace(SchematicBlock *startBB,
                                                     const SchematicSolution &solution);

/// Analyze all uncovered blocks by building synthetic traces and running analyzeTrace.
/// Reference: schematic.py:find_and_analyse_not_fixed_paths (line 504).
/// Returns true if all traces are feasible. On failure, errorMessage is set.
bool findAndAnalyzeNotFixedPaths(const CFGAnalysis &cfg, SchematicSolution &solution,
                                 const SchematicStateAnalysis &state, const SchematicParams &params,
                                 VMAddressTracker *tracker, llvm::LoopInfo &LI,
                                 llvm::Loop *loopScope, SchematicGraph &graph,
                                 std::string &errorMessage);

/// Single pass over CFG edges: enable checkpoints where allocations differ
/// or energy is insufficient, propagate energy otherwise.
/// When loopScope is non-null, only processes edges within that loop.
/// Reference: schematic.py:remove_potential_checkpoints_between_fixed_bbs (line 468).
void removePotentialCheckpointsBetweenFixedBBs(const CFGAnalysis &cfg, SchematicSolution &solution,
                                               const SchematicStateAnalysis &state,
                                               const SchematicParams &params, llvm::LoopInfo &LI,
                                               SchematicGraph &graph,
                                               llvm::Loop *loopScope = nullptr);

} // namespace checkpoint
