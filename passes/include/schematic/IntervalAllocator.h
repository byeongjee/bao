#pragma once

#include "common/CFGAnalysis.h"
#include "milp/StateAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"

#include <map>
#include <utility>
#include <vector>

namespace checkpoint {

/// Compute live_start and live_end flags for variable v in an interval.
/// live_start: 1 if first access to v is a read (needs restore at interval start)
/// live_end: 1 if v is live-out of interval (needs save at interval end)
std::pair<bool, bool>
computeLivenessFlags(llvm::GlobalVariable *v, const std::vector<llvm::BasicBlock *> &intervalBlocks,
                     const StateAnalysis &state,
                     const std::vector<llvm::BasicBlock *> *postIntervalBlocks = nullptr);

/// Compute optimal greedy allocation for an interval (spec §6.2).
RegionAllocation
computeIntervalAllocation(const std::vector<llvm::BasicBlock *> &intervalBlocks,
                          const StateAnalysis &state, const SchematicParams &params,
                          const std::map<llvm::GlobalVariable *, Placement> &fixedPlacements = {},
                          const std::vector<llvm::BasicBlock *> *postIntervalBlocks = nullptr);

/// Compute total interval energy (spec §7.2).
double computeIntervalEnergy(const std::vector<llvm::BasicBlock *> &intervalBlocks,
                             const RegionAllocation &allocation, const StateAnalysis &state,
                             const CFGAnalysis &cfg, const SchematicParams &params,
                             bool isFirstInterval, bool isLastInterval,
                             const std::vector<llvm::BasicBlock *> *postIntervalBlocks = nullptr);

} // namespace checkpoint
