#include "CheckpointAnalysisPass.h"
#include "CFGAnalysis.h"
#include "CheckpointOptimizer.h"
#include "EnergyEstimatorFactory.h"
#include "LoopTripCount.h"
#include "MaxCheckpointCounter.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

// Reference the energy config option from CheckpointPass.cpp
extern cl::opt<std::string> EnergyConfigOpt;

namespace {

// Command line options for analysis pass
static cl::opt<unsigned> DefaultBoundOpt(
    "analysis-default-bound",
    cl::desc("Default loop trip count for unannotated loops"),
    cl::init(2));

} // anonymous namespace

namespace checkpoint {

PreservedAnalyses CheckpointAnalysisPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
    // Validate required config
    if (EnergyConfigOpt.getValue().empty()) {
        errs() << "Error: -energy-config is required for checkpoint-analysis pass\n";
        return PreservedAnalyses::all();
    }

    // Create energy estimator from config
    auto estimator = EnergyEstimatorFactory::instance().createFromConfig(
        EnergyConfigOpt.getValue());
    if (!estimator) {
        errs() << "Failed to create energy estimator\n";
        return PreservedAnalyses::all();
    }

    // Skip declarations
    if (F.isDeclaration()) {
        return PreservedAnalyses::all();
    }

    double capacity = estimator->getCapacity();

    // Step 1: Get loop info from LLVM
    auto &LI = AM.getResult<LoopAnalysis>(F);

    // Step 2: Analyze CFG and run checkpoint optimizer
    CFGAnalysis cfg(F, LI, *estimator);

    // Check feasibility
    CheckpointOptimizer optimizer(cfg, capacity);
    auto infeasible = optimizer.getInfeasibleBlocks();
    if (!infeasible.empty()) {
        errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
        errs() << "Error: The following blocks exceed energy capacity:\n";
        for (const auto &block : infeasible) {
            errs() << "  " << block << " (cost: "
                   << cfg.getBlockInfo(block).energyCost
                   << ", capacity: " << capacity << ")\n";
        }
        return PreservedAnalyses::all();
    }

    // Solve MILP
    if (!optimizer.solve()) {
        errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
        errs() << "Error: Optimization failed\n";
        return PreservedAnalyses::all();
    }

    // Get checkpoint block names
    auto checkpointNames = optimizer.getCheckpoints();

    // Convert names to BasicBlock pointers
    std::set<BasicBlock*> checkpoints;
    for (BasicBlock &BB : F) {
        std::string blockName = BB.getName().str();
        if (blockName.empty()) {
            // Handle unnamed blocks
            size_t idx = 0;
            for (BasicBlock &B : F) {
                if (&B == &BB) {
                    blockName = "bb" + std::to_string(idx);
                    break;
                }
                idx++;
            }
        }
        if (checkpointNames.count(blockName)) {
            checkpoints.insert(&BB);
        }
    }

    // Step 3: Extract loop bounds from __loop_tripcount markers
    auto loopBounds = LoopTripCount::extractBounds(F, LI);

    // Step 4: Count max checkpoints using DP
    MaxCheckpointCounter counter(F, LI, checkpoints);
    counter.setLoopBounds(loopBounds);
    counter.setDefaultBound(DefaultBoundOpt);
    CountResult result = counter.compute();

    // Step 5: Output results
    errs() << "=== Checkpoint Analysis: " << F.getName() << " ===\n";
    errs() << "Configuration:\n";
    errs() << "  Energy capacity: " << capacity << "\n";
    errs() << "  Default loop bound: " << DefaultBoundOpt << "\n";
    errs() << "\n";

    errs() << "Checkpoints placed (from MILP): " << checkpointNames.size()
           << " block(s)\n";
    if (!checkpointNames.empty()) {
        errs() << "  ";
        bool first = true;
        for (const auto &name : checkpointNames) {
            if (!first) errs() << ", ";
            errs() << name;
            first = false;
        }
        errs() << "\n";
    }
    errs() << "\n";

    errs() << "Loop bounds:\n";
    if (loopBounds.empty()) {
        errs() << "  (none annotated, using default: " << DefaultBoundOpt << ")\n";
    } else {
        for (const auto &[loop, bound] : loopBounds) {
            BasicBlock *Header = loop->getHeader();
            errs() << "  Loop @ " << Header->getName() << ": " << bound
                   << " iterations (from __loop_tripcount)\n";
        }
    }
    errs() << "\n";

    errs() << "Result:\n";
    errs() << "  Maximum checkpoints on any path: " << result.maxCheckpoints
           << "\n";

    if (!result.criticalPath.empty()) {
        errs() << "  Critical path: ";
        for (size_t i = 0; i < result.criticalPath.size() && i < 10; ++i) {
            if (i > 0) errs() << " -> ";
            errs() << result.criticalPath[i]->getName();
        }
        if (result.criticalPath.size() > 10) {
            errs() << " -> ... (" << (result.criticalPath.size() - 10)
                   << " more)";
        }
        errs() << "\n";
    }

    errs() << "\n";
    errs() << "WARNING: __loop_tripcount markers may affect optimization.\n";
    errs() << "         Consider running analysis on -O0 builds.\n";
    errs() << "\n";

    // Analysis pass doesn't modify IR
    return PreservedAnalyses::all();
}

} // namespace checkpoint

// Plugin registration - extends the existing registration
// Note: This file should be compiled together with CheckpointPass.cpp
// The plugin registration in CheckpointPass.cpp will be updated to include both passes
