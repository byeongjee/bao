#pragma once

#include "common/CFGAnalysis.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"
#include "schematic/SchematicStateAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"

namespace checkpoint {

/// Compute block execution energy adjusted for VM placement savings.
/// Matches reference's bb.final_cost.
double getBlockExecEnergy(llvm::BasicBlock *BB, const SchematicSolution &solution,
                          const CFGAnalysis &cfg, const SchematicStateAnalysis &state,
                          const SchematicParams &params);

/// Backward energy propagation (reference: cfg_modification.py:171-253).
void propagateEnergyToLeave(const CFGEdge &seedEdge, double seedEToLeave,
                            SchematicSolution &solution, const CFGAnalysis &cfg,
                            const SchematicStateAnalysis &state, const SchematicParams &params,
                            llvm::LoopInfo &LI, llvm::Loop *loopScope);

/// Forward energy propagation (reference: cfg_modification.py:256-317).
void propagateEnergyLeft(const CFGEdge &seedEdge, double seedELeft, SchematicSolution &solution,
                         const CFGAnalysis &cfg, const SchematicStateAnalysis &state,
                         const SchematicParams &params, llvm::LoopInfo &LI, llvm::Loop *loopScope);

} // namespace checkpoint
