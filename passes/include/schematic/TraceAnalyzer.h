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

} // namespace checkpoint
