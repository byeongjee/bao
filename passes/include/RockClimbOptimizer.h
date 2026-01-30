#pragma once

#include "CFGAnalysis.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace checkpoint {

/// Information about a region formed by RockClimb partitioning.
struct RegionInfo {
    std::string startBlock;                    // First block of the region
    std::vector<std::string> blocks;           // All blocks in the region
    std::set<llvm::Value*> liveOutRegisters;   // Registers live at region exit
    double totalEnergy;                        // Total energy of the region
};

/// RockClimb optimizer: greedy region partitioning.
/// Partitions CFG into regions where WCET(region) < E_safe.
/// Unlike MILP, uses topological traversal with energy accumulation.
class RockClimbOptimizer {
public:
    /// Result of optimization.
    struct Result {
        std::vector<std::string> regionBoundaries;  // Blocks starting new regions
        std::vector<RegionInfo> regions;            // Detailed region information
        bool feasible;                              // True if partitioning succeeded
        std::string errorMessage;                   // Error description if not feasible
    };

    /// Construct optimizer for a CFG.
    /// @param cfg The CFG analysis results.
    /// @param E_safe Maximum safe energy per region.
    /// @param LI Loop info for mandatory header boundaries.
    RockClimbOptimizer(const CFGAnalysis &cfg, double E_safe, llvm::LoopInfo &LI);

    /// Run the optimization algorithm.
    /// @return Result containing region boundaries and info.
    Result optimize();

    /// Get blocks that exceed E_safe individually (infeasible).
    std::vector<std::string> getInfeasibleBlocks() const;

private:
    const CFGAnalysis &cfg_;
    double E_safe_;
    llvm::LoopInfo &LI_;

    /// Topologically sorted blocks.
    std::vector<std::string> topoOrder_;

    /// Map from block name to its BasicBlock*.
    std::map<std::string, llvm::BasicBlock*> blockMap_;

    /// Loop headers (mandatory region boundaries).
    std::set<std::string> loopHeaders_;

    /// Build topological order of blocks.
    void computeTopologicalOrder();

    /// Identify loop headers from LoopInfo.
    void identifyLoopHeaders();

    /// Main partitioning algorithm (Algorithm 1 from RockClimb paper).
    Result partitionRegions();
};

} // namespace checkpoint
