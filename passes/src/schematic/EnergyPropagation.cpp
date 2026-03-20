// passes/src/schematic/EnergyPropagation.cpp
#include "schematic/EnergyPropagation.h"

#include "llvm/IR/CFG.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>

namespace checkpoint {

// Static helper: compute block execution energy adjusted for VM placement savings.
// Matches reference's bb.final_cost. Must NOT be called on synthetic boundary blocks.
static double getBlockExecEnergy(llvm::BasicBlock *BB, const SchematicSolution &solution,
                                 const CFGAnalysis &cfg, const SchematicStateAnalysis &state,
                                 const SchematicParams &params) {
    double E = cfg.getBlockInfo(BB).energyCost;
    auto allocIt = solution.decidedPlacements.find(BB);
    if (allocIt != solution.decidedPlacements.end()) {
        for (const auto &[gv, place] : allocIt->second) {
            if (place != Placement::VM)
                continue;
            unsigned loads = state.getLoadCount(BB, gv);
            unsigned stores = state.getStoreCount(BB, gv);
            E -= params.nvmAccessPenalty * (loads + stores);
        }
    }
    return E;
}

void propagateEnergyToLeave(const CFGEdge &seedEdge, double seedEToLeave,
                            SchematicSolution &solution, const CFGAnalysis &cfg,
                            const SchematicStateAnalysis &state, const SchematicParams &params,
                            llvm::LoopInfo &LI, llvm::Loop *loopScope) {
    // TODO: implement in Task 2
}

void propagateEnergyLeft(const CFGEdge &seedEdge, double seedELeft, SchematicSolution &solution,
                         const CFGAnalysis &cfg, const SchematicStateAnalysis &state,
                         const SchematicParams &params, llvm::LoopInfo &LI, llvm::Loop *loopScope) {
    // TODO: implement in Task 3
}

} // namespace checkpoint
