#include "schematic/SchematicPass.h"

#include "common/BlockSplitter.h"
#include "milp/CheckpointContext.h"
#include "milp/StateAnalysis.h"
#include "schematic/IntervalAllocator.h"
#include "schematic/LoopAnalyzer.h"
#include "schematic/PathEnumerator.h"
#include "schematic/RCGSolver.h"
#include "schematic/SchematicInstrumenter.h"
#include "schematic/SchematicParams.h"
#include "schematic/SchematicSolution.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"

#include <chrono>
#include <cstddef>

using namespace llvm;

// Defined in src/common/PassRegistry.cpp
extern cl::opt<std::string> EnergyConfigOpt;
extern cl::opt<std::string> SchematicConfigOpt;
extern cl::opt<bool> AddDebugMarkersOpt;

namespace checkpoint {

PreservedAnalyses SchematicPass::run(Function &F,
                                     FunctionAnalysisManager &AM) {
    const auto totalStart = std::chrono::steady_clock::now();

    // Step 1: Obtain LLVM analyses
    auto &LI = AM.getResult<LoopAnalysis>(F);
    auto &AA = AM.getResult<AAManager>(F);
    auto &BFI = AM.getResult<BlockFrequencyAnalysis>(F);
    (void)BFI;

    // Step 2: Create base checkpoint context (estimator + CFG)
    auto ctxResult = createCheckpointContext(F, LI, EnergyConfigOpt.getValue(),
                                             "schematic pass");
    if (!ctxResult.success()) {
        if (!ctxResult.shouldSkip()) {
            errs() << ctxResult.errorMessage;
        }
        return PreservedAnalyses::all();
    }

    auto &ctx = *ctxResult.context;

    // Step 2b: Hoist non-entry static allocas to the entry block.
    BasicBlock &entryBB = F.getEntryBlock();
    for (BasicBlock &BB : F) {
        if (&BB == &entryBB)
            continue;
        for (auto it = BB.begin(); it != BB.end(); ) {
            auto *AI = dyn_cast<AllocaInst>(&*it++);
            if (AI && isa<ConstantInt>(AI->getArraySize()))
                AI->moveBefore(entryBB, entryBB.getFirstInsertionPt());
        }
    }

    // Step 3: Run StateAnalysis
    ctx.stateAnalysis = std::make_unique<StateAnalysis>(F, AA, *ctx.cfg);
    if (ctx.stateAnalysis->hasAnalysisErrors()) {
        ctx.stateAnalysis->printAnalysisErrors(errs());
        errs() << "Skipping SCHEMATIC instrumentation for function "
               << F.getName() << " due to unresolved memory/call effects.\n";
        return PreservedAnalyses::all();
    }

    // Current SCHEMATIC implementation is intraprocedural.
    // Emit an explicit note when user-level calls are present.
    bool hasUserCalls = false;
    for (const BasicBlock &BB : F) {
        for (const Instruction &I : BB) {
            const auto *CB = dyn_cast<CallBase>(&I);
            if (!CB)
                continue;
            const Function *Callee = CB->getCalledFunction();
            if (!Callee || Callee->isIntrinsic())
                continue;
            if (Callee->getName().starts_with("__"))
                continue;
            hasUserCalls = true;
            break;
        }
        if (hasUserCalls)
            break;
    }
    if (hasUserCalls) {
        errs() << "Note: SCHEMATIC currently applies intraprocedural analysis "
               << "for function '" << F.getName()
               << "' (interprocedural call-graph constraints are deferred).\n";
    }

    // Step 4: Parse SCHEMATIC config
    auto paramsOpt = parseSchematicParams(SchematicConfigOpt.getValue());
    if (!paramsOpt) {
        errs() << "Error: Failed to parse SCHEMATIC config: "
               << SchematicConfigOpt.getValue() << "\n";
        return PreservedAnalyses::all();
    }
    const auto &params = *paramsOpt;

    // Count ineligible objects by type
    unsigned ineligGlobalCount = 0, ineligAllocaCount = 0, ineligSSACount = 0;
    for (Value *V : ctx.stateAnalysis->getIneligibleObjs()) {
        if (isa<GlobalVariable>(V))
            ineligGlobalCount++;
        else if (isa<AllocaInst>(V))
            ineligAllocaCount++;
        else
            ineligSSACount++;
    }

    // Step 5: Split oversized blocks.
    // Conservative precondition: a single-block interval must still leave room
    // for checkpoint boundary costs (prologue/epilogue + regs), otherwise
    // RCG can become spuriously infeasible at low capacities.
    double minCheckpointOverhead =
        params.E_pro + params.E_epi +
        params.N_reg * (params.regStoreEnergy + params.regRestoreEnergy);
    double splitThreshold = params.capacity - minCheckpointOverhead;
    if (!splitAllOversizedBlocks(F, splitThreshold, *ctx.estimator,
                                  LI, *ctx.cfg)) {
        errs() << "Error: unsplittable block exceeds capacity in "
               << F.getName() << "\n";
        return PreservedAnalyses::all();
    }

    // Block splitting adds new BasicBlocks not registered in LoopInfo or
    // ScalarEvolution. Invalidate all cached analyses and re-obtain them.
    {
        PreservedAnalyses PA = PreservedAnalyses::none();
        AM.invalidate(F, PA);
    }
    auto &LI2 = AM.getResult<LoopAnalysis>(F);
    auto &SE2 = AM.getResult<ScalarEvolutionAnalysis>(F);
    auto &AA2 = AM.getResult<AAManager>(F);

    ctx.cfg = std::make_unique<CFGAnalysis>(F, LI2, *ctx.estimator);
    ctx.stateAnalysis = std::make_unique<StateAnalysis>(F, AA2, *ctx.cfg);

    // Step 6: Analyze loops bottom-up
    SchematicSolution solution;
    LoopAnalyzer loopAnalyzer(LI2, SE2, *ctx.cfg, *ctx.stateAnalysis, params);
    if (!loopAnalyzer.analyzeLoops(solution)) {
        return PreservedAnalyses::all();
    }

    // Record loop body allocations as decided placements for subsequent paths
    for (const auto &[header, decision] : solution.loopDecisions) {
        if (!decision.loop) continue;
        for (llvm::BasicBlock *BB : decision.loop->getBlocks()) {
            auto &decided = solution.decidedPlacements[BB];
            for (const auto &[gv, p] : decision.bodyAllocation.placement)
                decided.insert({gv, p}); // first decision wins
        }
    }

    // Step 7: Enumerate paths
    auto &BPI = AM.getResult<BranchProbabilityAnalysis>(F);
    PathEnumerator pathEnum(F, BPI, LI2, params.maxPaths);
    auto paths = pathEnum.enumerate();

    // Step 8: Analyze paths via RCG
    for (const auto &path : paths) {
        auto isAnalyzed = [&](llvm::BasicBlock *BB) {
            auto metaIt = solution.blockMeta.find(BB);
            return metaIt != solution.blockMeta.end() && metaIt->second.analyzed;
        };

        // Analyze only maximal contiguous segments containing unprocessed blocks.
        std::vector<std::pair<size_t, size_t>> segments;
        size_t idx = 0;
        while (idx < path.blocks.size()) {
            while (idx < path.blocks.size() && isAnalyzed(path.blocks[idx]))
                ++idx;
            if (idx >= path.blocks.size())
                break;
            size_t start = idx;
            while (idx < path.blocks.size() && !isAnalyzed(path.blocks[idx]))
                ++idx;
            segments.push_back({start, idx}); // [start, idx)
        }
        if (segments.empty())
            continue;

        for (const auto &[segStart, segEnd] : segments) {
            std::vector<llvm::BasicBlock *> segBlocks(
                path.blocks.begin() + static_cast<std::ptrdiff_t>(segStart),
                path.blocks.begin() + static_cast<std::ptrdiff_t>(segEnd));

            llvm::BasicBlock *startBoundary =
                (segStart > 0) ? path.blocks[segStart - 1] : nullptr;
            llvm::BasicBlock *endBoundary =
                (segEnd < path.blocks.size()) ? path.blocks[segEnd] : nullptr;

            RCGSolver rcg(segBlocks, *ctx.stateAnalysis, *ctx.cfg, params,
                          solution.blockMeta, solution.decidedPlacements,
                          startBoundary, endBoundary);
            auto result = rcg.solve();
            if (!result.feasible) {
                errs() << "Warning: infeasible path segment in " << F.getName()
                       << ": " << result.errorMessage << "\n";
                return PreservedAnalyses::all();
            }

            // Record decisions
            for (const auto &edge : result.selectedCheckpoints)
                solution.enabledCheckpoints.insert(edge);
            for (size_t i = 0; i < result.allocations.size(); ++i) {
                solution.regions.push_back(
                    {result.intervalBlocks[i], result.allocations[i]});
                // Record decided placements for subsequent analyses
                for (llvm::BasicBlock *BB : result.intervalBlocks[i]) {
                    auto &decided = solution.decidedPlacements[BB];
                    for (const auto &[gv, p] : result.allocations[i].placement)
                        decided.insert({gv, p}); // first decision wins
                }
            }

            // Update blockMeta (E_left, E_to_leave, analyzed) — monotonic
            double segmentBudget = params.capacity;
            if (startBoundary) {
                auto it = solution.blockMeta.find(startBoundary);
                if (it != solution.blockMeta.end())
                    segmentBudget = std::min(segmentBudget, it->second.E_left);
            }

            double tailLeave = 0.0;
            if (endBoundary) {
                auto it = solution.blockMeta.find(endBoundary);
                if (it != solution.blockMeta.end())
                    tailLeave = std::max(0.0, it->second.E_to_leave);
            }

            double cumulativeEnergy = 0.0;
            for (size_t ri = 0; ri < result.intervalBlocks.size(); ++ri) {
                cumulativeEnergy = 0.0;
                for (llvm::BasicBlock *BB : result.intervalBlocks[ri]) {
                    double blockE = ctx.cfg->getBlockInfo(BB).energyCost;
                    for (const auto &[gv, p] : result.allocations[ri].placement) {
                        if (p == Placement::NVM) {
                            unsigned accesses =
                                ctx.stateAnalysis->getLoadCount(BB, gv) +
                                ctx.stateAnalysis->getStoreCount(BB, gv);
                            blockE += accesses * params.nvmAccessPenalty;
                        }
                    }
                    cumulativeEnergy += blockE;

                    auto &meta = solution.blockMeta[BB];
                    double newELeft = segmentBudget - cumulativeEnergy;
                    double newEToLeave = cumulativeEnergy + tailLeave;
                    if (newELeft < meta.E_left)
                        meta.E_left = newELeft;
                    if (newEToLeave > meta.E_to_leave)
                        meta.E_to_leave = newEToLeave;
                    meta.analyzed = true;
                }
            }
            solution.pathsAnalyzed++;
        }
    }

    // Check for incomplete coverage
    for (BasicBlock &BB : F) {
        auto it = solution.blockMeta.find(&BB);
        if (it == solution.blockMeta.end() || !it->second.analyzed) {
            errs() << "Warning: incomplete SCHEMATIC coverage in "
                   << F.getName() << ": block '" << BB.getName()
                   << "' was not analyzed.\n";
        }
    }

    // Compute VM/NVM variable counts
    std::set<llvm::GlobalVariable *> vmVarSet, nvmVarSet;
    for (const auto &region : solution.regions) {
        for (const auto &[gv, p] : region.allocation.placement) {
            if (p == Placement::VM)
                vmVarSet.insert(gv);
            else
                nvmVarSet.insert(gv);
        }
    }
    solution.totalVmVariables = vmVarSet.size();
    solution.totalNvmVariables = nvmVarSet.size();

    // Step 9: Instrument
    bool addDebugMarkers =
        AddDebugMarkersOpt.getValue() || params.addDebugMarkers;

    SchematicInstrumenter instrumenter(*F.getParent(), addDebugMarkers,
                                       params.N_reg);
    unsigned runtimeCallsInserted = instrumenter.instrumentFunction(
        F, solution, *ctx.stateAnalysis);

    unsigned regionsPlaced = solution.regions.size();
    unsigned checkpointsPlaced = solution.enabledCheckpoints.size();
    unsigned pathsAnalyzed = solution.pathsAnalyzed;
    unsigned vmVariables = solution.totalVmVariables;
    unsigned nvmVariables = solution.totalNvmVariables;

    const auto totalEnd = std::chrono::steady_clock::now();
    double totalExecutionTimeMs =
        std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();

    // Statistics summary
    errs() << "=== SCHEMATIC Checkpoint Insertion Statistics ===\n";
    errs() << "  Basic blocks:                  " << ctx.cfg->getBlocks().size()
           << "\n";
    errs() << "  Edges:                         " << ctx.cfg->getEdges().size()
           << "\n";
    errs() << "  Candidate globals (V_elig):    "
           << ctx.stateAnalysis->getVMObjs().size() << "\n";
    errs() << "  Ineligible globals:            " << ineligGlobalCount << "\n";
    errs() << "  Ineligible allocas:            " << ineligAllocaCount << "\n";
    errs() << "  Ineligible SSA registers:      " << ineligSSACount << "\n";
    errs() << "  Regions:                       " << regionsPlaced << "\n";
    errs() << "  Checkpoints placed:            " << checkpointsPlaced << "\n";
    errs() << "  Paths analyzed:                " << pathsAnalyzed << "\n";
    errs() << "  Variables placed in VM:        " << vmVariables << "\n";
    errs() << "  Variables placed in NVM:       " << nvmVariables << "\n";
    errs() << "  Runtime calls inserted:        " << runtimeCallsInserted << "\n";
    errs() << "  Total execution time (ms):     "
           << format("%.3f", totalExecutionTimeMs) << "\n";

    if (runtimeCallsInserted > 0)
        return PreservedAnalyses::none();
    return PreservedAnalyses::all();
}

} // namespace checkpoint
