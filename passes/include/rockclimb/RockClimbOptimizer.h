#pragma once

#include "common/CFGAnalysis.h"
#include "estimator/EnergyEstimator.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

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
/// Implements Algorithm 1 from the RockClimb paper with path-aware
/// energy accumulation using per-block IncomeCycle propagation.
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
    /// @param F The function being optimized.
    /// @param estimator Energy estimator for per-instruction costs (for block splitting).
    RockClimbOptimizer(const CFGAnalysis &cfg, double E_safe, llvm::LoopInfo &LI,
                       llvm::Function &F, EnergyEstimator *estimator = nullptr);

    /// Run the optimization algorithm.
    /// @return Result containing region boundaries and info.
    Result optimize();

    /// Get blocks that exceed E_safe individually (infeasible).
    std::vector<std::string> getInfeasibleBlocks() const;

    /// Set extra block costs from checkpoint stores (CkptCycles_bbi).
    /// Used for iterative refinement: partition -> analyze ckpts -> add costs -> re-partition.
    /// @param costs Map from block name to additional energy cost.
    void setExtraBlockCosts(const std::map<std::string, double> &costs);

private:
    const CFGAnalysis &cfg_;
    double E_safe_;
    llvm::LoopInfo &LI_;
    llvm::Function &F_;
    EnergyEstimator *estimator_;

    /// Topologically sorted blocks.
    std::vector<std::string> topoOrder_;

    /// Map from block name to its BasicBlock*.
    std::map<std::string, llvm::BasicBlock*> blockMap_;

    /// Loop headers (mandatory region boundaries).
    std::set<std::string> loopHeaders_;

    /// Blocks containing function calls (mandatory region boundaries).
    std::set<std::string> callSiteBlocks_;

    /// Extra block costs from checkpoint stores (CkptCycles_bbi).
    std::map<std::string, double> extraBlockCosts_;

    /// Successor adjacency map (block -> list of successors).
    std::map<std::string, std::vector<std::string>> successors_;

    /// Build topological order of blocks.
    void computeTopologicalOrder();

    /// Identify loop headers from LoopInfo.
    void identifyLoopHeaders();

    /// Identify blocks containing function calls to non-intrinsic functions.
    void identifyCallSiteBlocks();

    /// Build successor adjacency map from CFG edges.
    void buildAdjacencyMaps();

    /// Get effective block cost: Cycle_ori + CkptCycles.
    double getBlockCost(const std::string &block) const;

    /// Main partitioning algorithm (Algorithm 1 from RockClimb paper).
    Result partitionRegions();

    /// Split a block when its cost exceeds threshold (Algorithm 1 while loop).
    /// Splits the LLVM BasicBlock at the instruction where accumulated cost
    /// reaches the threshold, updates local data structures.
    /// @param blockName Name of the block to split.
    /// @param threshold Energy threshold (E_safe).
    /// @param insertIdx Index in topoOrder_ after which the new block is inserted.
    /// @return Name of the new (second half) block, or empty if split not possible.
    std::string splitBlock(const std::string &blockName, double threshold,
                           size_t insertIdx);
};

} // namespace checkpoint
