#include "schematic/LoopAnalyzer.h"
#include "common/Logger.h"
#include "common/LoopTripCount.h"
#include "schematic/EnergyPropagation.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/RCGSolver.h"
#include "schematic/TraceAnalyzer.h"

#include "llvm/IR/CFG.h"
#include "llvm/Support/ErrorHandling.h"

#include <cmath>
#include <deque>
#include <limits>

namespace checkpoint {

static std::string loopOriginTag(llvm::Loop *L, llvm::StringRef reason) {
    llvm::BasicBlock *header = L ? L->getHeader() : nullptr;
    std::string headerName = header ? header->getName().str() : "<unknown>";
    return ("loop-" + reason + "[" + headerName + "]").str();
}

static void recomputeLoopBodyEnergyOnCFG(llvm::Loop *L, RegionAllocation &bodyAlloc,
                                         SchematicSolution &solution, const CFGAnalysis &cfg,
                                         const SchematicStateAnalysis &state,
                                         const SchematicParams &params, SchematicGraph &graph,
                                         SchematicBlock *headerBlock, SchematicBlock *latchBlock,
                                         SchematicBlock *startSynth, SchematicBlock *endSynth);

static std::map<llvm::Value *, VariableAccessEstimate>
estimateLoopVariableAccess(const LoadedLoopTrace &loopTrace, const SchematicStateAnalysis &state,
                           uint64_t numIterations);

namespace {

struct LoopBoundaryBlocks {
    llvm::BasicBlock *header;
    llvm::BasicBlock *latch;
    SchematicBlock *headerBlock;
    SchematicBlock *latchBlock;
    SchematicBlock *startSynth;
    SchematicBlock *endSynth;
};

struct LoopBoundaryPlacements {
    std::map<llvm::Value *, Placement> headerAlloc;
    std::map<llvm::Value *, Placement> latchAlloc;
    bool allocationsDiffer = false;
};

struct LoopIterationBudget {
    double startEToLeave = 0.0;
    double endEToLeave = 0.0;
    double ELoop = 0.0;
    double availableEnergy = 0.0;
    int rawNumIterations = 0;
    unsigned numIterations = 0;
};

static LoopBoundaryBlocks createLoopBoundaryBlocks(llvm::Loop *L, SchematicGraph &graph) {
    llvm::BasicBlock *header = L->getHeader();
    llvm::BasicBlock *latch = L->getLoopLatch();
    return LoopBoundaryBlocks{
        header,
        latch,
        graph.getOrCreate(header),
        latch ? graph.getOrCreate(latch) : nullptr,
        graph.createSynthetic(kStartLoopName.str()),
        graph.createSynthetic(kEndLoopName.str()),
    };
}

static void eraseSyntheticLoopBoundaryState(SchematicSolution &solution,
                                            const LoopBoundaryBlocks &blocks) {
    solution.blockMeta.erase(blocks.startSynth);
    solution.blockMeta.erase(blocks.endSynth);
    solution.decidedPlacements.erase(blocks.startSynth);
    solution.decidedPlacements.erase(blocks.endSynth);
    solution.blockAllocation.erase(blocks.startSynth);
    solution.blockAllocation.erase(blocks.endSynth);
}

static double computeLoopEntryEnergyLeft(const RegionAllocation &alloc,
                                         const SchematicStateAnalysis &state,
                                         const SchematicParams &params) {
    double energyLeft = params.capacity - params.E_pro - params.N_reg * params.regRestoreEnergy;
    for (const auto &[gv, va] : alloc.vars) {
        if (va.placement == Placement::VM)
            energyLeft -= params.memRestoreEnergyPerByte * state.getVarSizeBytes(gv);
    }
    return energyLeft;
}

static double computeLoopExitEnergyToLeave(const RegionAllocation &alloc,
                                           const SchematicStateAnalysis &state,
                                           const SchematicParams &params,
                                           bool includeLoopIncrementCost) {
    double eToLeave = params.E_epi + params.N_reg * params.regStoreEnergy;
    if (includeLoopIncrementCost)
        eToLeave += params.loopIncrementCostNvm;
    for (const auto &[gv, va] : alloc.vars) {
        if (va.placement == Placement::VM)
            eToLeave += params.memStoreEnergyPerByte * state.getVarSizeBytes(gv);
    }
    return eToLeave;
}

static void initializeSyntheticLoopBoundaryState(SchematicSolution &solution,
                                                 const SchematicStateAnalysis &state,
                                                 const SchematicParams &params,
                                                 const LoopBoundaryBlocks &blocks) {
    RegionAllocation emptyAlloc;
    solution.blockMeta[blocks.startSynth].E_left =
        computeLoopEntryEnergyLeft(emptyAlloc, state, params);
    solution.blockMeta[blocks.startSynth].analyzed = true;
    solution.decidedPlacements[blocks.startSynth] = {};

    solution.blockMeta[blocks.endSynth].E_to_leave =
        computeLoopExitEnergyToLeave(emptyAlloc, state, params, false);
    solution.blockMeta[blocks.endSynth].analyzed = true;
    solution.decidedPlacements[blocks.endSynth] = {};
}

static const LoadedLoopTrace *
findLoadedLoopTraceForHeader(SchematicBlock *headerBlock,
                             const std::vector<LoadedLoopTrace> &loadedLoopTraces) {
    for (const auto &lt : loadedLoopTraces) {
        if (lt.header == headerBlock)
            return &lt;
    }
    return nullptr;
}

static std::vector<std::vector<SchematicBlock *>>
collectLoopBodyPaths(const LoadedLoopTrace &loopTrace) {
    std::vector<std::vector<SchematicBlock *>> bodyPaths;
    for (const auto &ep : loopTrace.iterationPaths) {
        if (!ep.blocks.empty())
            bodyPaths.push_back(ep.blocks);
    }
    return bodyPaths;
}

static bool tracedLoopContainsBlock(const LoadedLoopTrace &loopTrace, SchematicBlock *block) {
    for (SchematicBlock *member : loopTrace.members) {
        if (member == block)
            return true;
    }
    return false;
}

static bool blockCoveredByNestedTracedLoop(const LoadedLoopTrace &parentLoopTrace,
                                           SchematicBlock *block,
                                           const std::vector<LoadedLoopTrace> &loadedLoopTraces) {
    if (!parentLoopTrace.loop)
        return false;

    for (const auto &candidate : loadedLoopTraces) {
        if (!candidate.loop || candidate.loop == parentLoopTrace.loop)
            continue;
        if (!parentLoopTrace.loop->contains(candidate.loop))
            continue;
        if (tracedLoopContainsBlock(candidate, block))
            return true;
    }
    return false;
}

static std::vector<std::string>
collectUnexplainedUncoveredLoopMembers(const LoadedLoopTrace &loopTrace,
                                       const std::vector<LoadedLoopTrace> &loadedLoopTraces) {
    std::set<SchematicBlock *> covered;
    for (const auto &ep : loopTrace.iterationPaths) {
        for (SchematicBlock *block : ep.blocks)
            covered.insert(block);
    }

    std::vector<std::string> uncovered;
    for (SchematicBlock *member : loopTrace.members) {
        if (covered.count(member))
            continue;
        if (blockCoveredByNestedTracedLoop(loopTrace, member, loadedLoopTraces))
            continue;
        uncovered.push_back(member->displayName());
    }
    return uncovered;
}

static std::string joinNames(const std::vector<std::string> &names) {
    std::string joined;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0)
            joined += ", ";
        joined += names[i];
    }
    return joined;
}

static std::vector<std::string>
collectLoopTraceCoverageErrors(llvm::LoopInfo &LI, SchematicGraph &graph,
                               const std::vector<LoadedLoopTrace> &loadedLoopTraces) {
    std::vector<std::string> errors;
    for (const auto &loopTrace : loadedLoopTraces) {
        std::vector<std::string> uncovered =
            collectUnexplainedUncoveredLoopMembers(loopTrace, loadedLoopTraces);
        if (uncovered.empty())
            continue;
        errors.push_back("loop trace for header '" + loopTrace.header->displayName() +
                         "' does not cover loop members without traced subloop coverage: " +
                         joinNames(uncovered));
    }

    for (llvm::Loop *L : LI.getLoopsInPreorder()) {
        SchematicBlock *headerBlock = graph.getOrCreate(L->getHeader());
        if (findLoadedLoopTraceForHeader(headerBlock, loadedLoopTraces))
            continue;

        llvm::Loop *tracedAncestor = nullptr;
        for (llvm::Loop *parent = L->getParentLoop(); parent; parent = parent->getParentLoop()) {
            SchematicBlock *parentHeaderBlock = graph.getOrCreate(parent->getHeader());
            if (findLoadedLoopTraceForHeader(parentHeaderBlock, loadedLoopTraces)) {
                tracedAncestor = parent;
                break;
            }
        }
        if (!tracedAncestor)
            continue;

        errors.push_back("loop at '" + L->getHeader()->getName().str() +
                         "' has no dedicated loop trace despite executing inside traced loop '" +
                         tracedAncestor->getHeader()->getName().str() + "'");
    }
    return errors;
}

static std::string joinLines(const std::vector<std::string> &lines) {
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i != 0)
            joined += "\n";
        joined += lines[i];
    }
    return joined;
}

static bool analyzeLoopBodyPaths(const std::vector<std::vector<SchematicBlock *>> &bodyPaths,
                                 const LoopBoundaryBlocks &blocks, SchematicSolution &solution,
                                 const SchematicStateAnalysis &state, const CFGAnalysis &cfg,
                                 const SchematicParams &params, VMAddressTracker *tracker,
                                 llvm::LoopInfo &LI, llvm::Loop *L, SchematicGraph &graph,
                                 std::string &errorMsg) {
    for (auto path : bodyPaths) {
        path.insert(path.begin(), blocks.startSynth);
        path.push_back(blocks.endSynth);
        graph.addTraceEdges(path);
        if (!analyzeTrace(path, solution, state, cfg, params, tracker, LI, L, errorMsg))
            return false;
    }
    return true;
}

static std::map<llvm::Value *, Placement> getDecidedPlacements(const SchematicSolution &solution,
                                                               SchematicBlock *block) {
    auto it = solution.decidedPlacements.find(block);
    if (it == solution.decidedPlacements.end())
        return {};
    return it->second;
}

static bool boundaryPlacementsDiffer(const std::map<llvm::Value *, Placement> &headerAlloc,
                                     const std::map<llvm::Value *, Placement> &latchAlloc) {
    for (const auto &[gv, place] : headerAlloc) {
        auto it = latchAlloc.find(gv);
        if (it != latchAlloc.end()) {
            if (it->second != place)
                return true;
        } else if (place == Placement::VM) {
            return true;
        }
    }

    for (const auto &[gv, place] : latchAlloc) {
        if (headerAlloc.find(gv) == headerAlloc.end() && place == Placement::VM)
            return true;
    }

    return false;
}

static void mergeBoundaryPlacements(std::map<llvm::Value *, Placement> &headerAlloc,
                                    std::map<llvm::Value *, Placement> &latchAlloc) {
    for (const auto &[gv, place] : latchAlloc) {
        if (headerAlloc.find(gv) == headerAlloc.end())
            headerAlloc[gv] = place;
    }
    for (const auto &[gv, place] : headerAlloc) {
        if (latchAlloc.find(gv) == latchAlloc.end())
            latchAlloc[gv] = place;
    }
}

static LoopBoundaryPlacements resolveLoopBoundaryPlacements(SchematicSolution &solution,
                                                            const LoopBoundaryBlocks &blocks) {
    LoopBoundaryPlacements placements;
    placements.headerAlloc = getDecidedPlacements(solution, blocks.headerBlock);
    if (blocks.latchBlock)
        placements.latchAlloc = getDecidedPlacements(solution, blocks.latchBlock);

    placements.allocationsDiffer =
        boundaryPlacementsDiffer(placements.headerAlloc, placements.latchAlloc);
    if (placements.allocationsDiffer)
        return placements;

    mergeBoundaryPlacements(placements.headerAlloc, placements.latchAlloc);
    solution.decidedPlacements[blocks.headerBlock] = placements.headerAlloc;
    if (blocks.latchBlock)
        solution.decidedPlacements[blocks.latchBlock] = placements.latchAlloc;
    return placements;
}

static void copyLoopBoundaryEnergyToSyntheticBlocks(SchematicSolution &solution,
                                                    const LoopBoundaryBlocks &blocks) {
    solution.blockMeta[blocks.startSynth].E_to_leave =
        solution.blockMeta[blocks.headerBlock].E_to_leave;
    solution.blockMeta[blocks.startSynth].E_left = solution.blockMeta[blocks.headerBlock].E_left;
    if (blocks.latchBlock)
        solution.blockMeta[blocks.endSynth].E_left = solution.blockMeta[blocks.latchBlock].E_left;
}

static LoopIterationBudget readLoopIterationBudget(const LoopBoundaryBlocks &blocks,
                                                   const SchematicParams &params,
                                                   const SchematicSolution &solution) {
    LoopIterationBudget budget;
    budget.startEToLeave = solution.blockMeta.at(blocks.startSynth).E_to_leave;
    budget.endEToLeave = solution.blockMeta.at(blocks.endSynth).E_to_leave;
    budget.ELoop = budget.startEToLeave - budget.endEToLeave + params.loopIncrementCostNvm;
    budget.availableEnergy = params.capacity - budget.endEToLeave;
    if (budget.ELoop > 0.0) {
        budget.rawNumIterations =
            static_cast<int>(std::floor(budget.availableEnergy / budget.ELoop)) - 1;
        budget.numIterations = static_cast<unsigned>(std::max(budget.rawNumIterations, 0));
    }
    return budget;
}

static bool hasEnabledCheckpointsInLoop(const SchematicSolution &solution, llvm::Loop *L) {
    for (const auto &ckpt : solution.enabledCheckpoints) {
        if (ckpt.src->getLLVMBlock() && L->contains(ckpt.src->getLLVMBlock()) &&
            ckpt.dst->getLLVMBlock() && L->contains(ckpt.dst->getLLVMBlock())) {
            return true;
        }
    }
    return false;
}

static bool hasOnlyDisabledCheckpointsInLoop(const SchematicSolution &solution, llvm::Loop *L,
                                             const CFGAnalysis &cfg, SchematicGraph &graph,
                                             const LoopBoundaryBlocks &blocks) {
    llvm::BasicBlock *loopLatch = L->getLoopLatch();
    llvm::BasicBlock *loopHeader = L->getHeader();
    for (const auto &cfgEdge : cfg.getEdges()) {
        auto *srcBB = const_cast<llvm::BasicBlock *>(cfgEdge.first);
        auto *dstBB = const_cast<llvm::BasicBlock *>(cfgEdge.second);
        if (!L->contains(srcBB) || !L->contains(dstBB))
            continue;
        if (srcBB == loopLatch && dstBB == loopHeader)
            continue;

        CFGEdge edge{graph.getOrCreate(srcBB), graph.getOrCreate(dstBB)};
        if (!isDisabledCheckpoint(solution, edge))
            return false;
    }

    for (SchematicBlock *succ : blocks.startSynth->successors()) {
        llvm::BasicBlock *succBB = succ ? succ->getLLVMBlock() : nullptr;
        if (!succBB || !L->contains(succBB))
            continue;
        if (!isDisabledCheckpoint(solution, CFGEdge{blocks.startSynth, succ}))
            return false;
    }

    for (SchematicBlock *pred : blocks.endSynth->predecessors()) {
        llvm::BasicBlock *predBB = pred ? pred->getLLVMBlock() : nullptr;
        if (!predBB || !L->contains(predBB))
            continue;
        if (!isDisabledCheckpoint(solution, CFGEdge{pred, blocks.endSynth}))
            return false;
    }

    return true;
}

static std::vector<const RegionAllocation *>
collectLoopMemoryAllocations(llvm::Loop *L, const SchematicSolution &solution,
                             SchematicGraph &graph) {
    std::vector<const RegionAllocation *> allocations;
    for (llvm::BasicBlock *BB : L->blocks()) {
        SchematicBlock *block = graph.getOrCreate(BB);
        auto it = solution.blockAllocation.find(block);
        if (it == solution.blockAllocation.end())
            continue;
        const RegionAllocation *ptr = it->second.get();
        if (std::find(allocations.begin(), allocations.end(), ptr) == allocations.end())
            allocations.push_back(ptr);
    }
    return allocations;
}

static void propagateMandatoryLoopBoundary(llvm::Loop *L, const LoopBoundaryBlocks &blocks,
                                           const RegionAllocation &bodyAllocation,
                                           SchematicSolution &solution, const CFGAnalysis &cfg,
                                           const SchematicStateAnalysis &state,
                                           const SchematicParams &params, llvm::LoopInfo &LI) {
    double energyLeftStart = computeLoopEntryEnergyLeft(bodyAllocation, state, params);
    SchematicBlock *fwdDst =
        blocks.startSynth->successors().empty() ? nullptr : blocks.startSynth->successors()[0];
    if (fwdDst)
        propagateEnergyLeft(CFGEdge{blocks.startSynth, fwdDst}, energyLeftStart, solution, cfg,
                            state, params, LI, L);

    double eToLeave = computeLoopExitEnergyToLeave(bodyAllocation, state, params, true);
    SchematicBlock *bwdSrc =
        blocks.endSynth->predecessors().empty() ? nullptr : blocks.endSynth->predecessors()[0];
    if (bwdSrc)
        propagateEnergyToLeave(CFGEdge{bwdSrc, blocks.endSynth}, eToLeave, solution, cfg, state,
                               params, LI, L);

    solution.blockMeta[blocks.startSynth].E_to_leave =
        solution.blockMeta[blocks.headerBlock].E_to_leave;
    solution.blockMeta[blocks.startSynth].E_left = energyLeftStart;
    solution.blockMeta[blocks.endSynth].E_to_leave = eToLeave;
}

static void applyLoopIterationAdjustment(llvm::Loop *L, unsigned maxTripCount,
                                         const LoopCheckpointDecision &decision, double ELoop,
                                         SchematicSolution &solution, SchematicGraph &graph) {
    if (decision.numIterationsPerCharge <= 1)
        return;

    unsigned adjIter = decision.numIterationsPerCharge;
    if (decision.loopFitsEntirely)
        adjIter = maxTripCount;
    LoopMark mark{L, adjIter, ELoop};
    double adjustment = (adjIter - 1) * ELoop;
    for (llvm::BasicBlock *BB : L->blocks()) {
        SchematicBlock *block = graph.getOrCreate(BB);
        auto &meta = solution.blockMeta[block];
        meta.loop = mark;
        meta.E_to_leave += adjustment;
        meta.E_left -= adjustment;
    }
}

static void refineLoopBudgetWithConvergence(
    llvm::Loop *L, const LoadedLoopTrace &matchedLoopTrace, uint64_t maxTripCount,
    const LoopBoundaryBlocks &blocks, SchematicSolution &solution, const CFGAnalysis &cfg,
    const SchematicStateAnalysis &state, const SchematicParams &params, llvm::LoopInfo &LI,
    VMAddressTracker *tracker, SchematicGraph &graph, LoopIterationBudget &budget,
    RegionAllocation &bodyAlloc, LoopCheckpointDecision &decision) {
    bool hasEnabledCheckpoints = hasEnabledCheckpointsInLoop(solution, L);
    bool onlyDisabledCheckpoints =
        hasOnlyDisabledCheckpointsInLoop(solution, L, cfg, graph, blocks);
    decision.hadEnabledCheckpoints = hasEnabledCheckpoints;
    if (!onlyDisabledCheckpoints || budget.numIterations <= 1)
        return;

    decision.convergenceApplied = true;
    std::vector<const RegionAllocation *> loopMemoryAllocations =
        collectLoopMemoryAllocations(L, solution, graph);

    unsigned previousNumIterations = 0;
    for (unsigned iter = 0; iter < 15; ++iter) {
        decision.convergenceIterations = iter + 1;
        if (previousNumIterations != 0 && budget.numIterations >= previousNumIterations)
            break;
        previousNumIterations = budget.numIterations;

        unsigned scaledIterations = static_cast<unsigned>(
            std::min(static_cast<uint64_t>(budget.numIterations), maxTripCount));
        auto variableAccesses =
            estimateLoopVariableAccess(matchedLoopTrace, state, scaledIterations);

        auto [newAlloc, _gain] = chooseMemoryAllocation(variableAccesses, state, params, nullptr,
                                                        nullptr, loopMemoryAllocations, tracker);
        bodyAlloc = newAlloc;

        recomputeLoopBodyEnergyOnCFG(L, bodyAlloc, solution, cfg, state, params, graph,
                                     blocks.headerBlock, blocks.latchBlock, blocks.startSynth,
                                     blocks.endSynth);
        refreshDisabledCheckpointEnergy(cfg, solution, state, params, LI, graph, L);

        budget = readLoopIterationBudget(blocks, params, solution);
        decision.E_loop = budget.ELoop;
        decision.bodyAllocation = bodyAlloc;

        if (budget.ELoop <= 0.0)
            break;

        // Convergence uses startEToLeave rather than endEToLeave, matching the
        // Python reference loop-budget update.
        budget.availableEnergy = params.capacity - budget.startEToLeave;
        if (budget.availableEnergy <= 0.0)
            break;
        budget.rawNumIterations =
            static_cast<int>(std::floor(budget.availableEnergy / budget.ELoop)) - 1;
        budget.numIterations = static_cast<unsigned>(std::max(budget.rawNumIterations, 0));
    }
}

static void applyLoopCheckpointPolicy(llvm::Loop *L, unsigned maxTripCount,
                                      const LoopBoundaryBlocks &blocks,
                                      const LoopIterationBudget &budget,
                                      SchematicSolution &solution,
                                      LoopCheckpointDecision &decision) {
    if (budget.numIterations > maxTripCount) {
        // Entire loop fits — no checkpoint needed, but use maxTripCount for
        // energy scaling so propagation accounts for all iterations.
        decision.numIterationsPerCharge = maxTripCount;
        decision.loopFitsEntirely = true;
        if (blocks.latchBlock)
            disableCheckpoint(solution, CFGEdge{blocks.latchBlock, blocks.headerBlock});
        return;
    }

    if (budget.numIterations < 3) {
        // Too few iterations per charge — checkpoint every iteration.
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        if (blocks.latchBlock)
            enableCheckpoint(solution, CFGEdge{blocks.latchBlock, blocks.headerBlock},
                             loopOriginTag(L, "mandatory-backedge"));
        return;
    }

    // Conditional checkpoint every numIt iterations.
    decision.numIterationsPerCharge = budget.numIterations;
    if (blocks.latchBlock)
        setLoopLatchCheckpoint(solution, CFGEdge{blocks.latchBlock, blocks.headerBlock});
}

} // namespace

static void recomputeLoopBodyEnergyOnCFG(llvm::Loop *L, RegionAllocation &bodyAlloc,
                                         SchematicSolution &solution, const CFGAnalysis &cfg,
                                         const SchematicStateAnalysis &state,
                                         const SchematicParams &params, SchematicGraph &graph,
                                         SchematicBlock *headerBlock, SchematicBlock *latchBlock,
                                         SchematicBlock *startSynth, SchematicBlock *endSynth) {
    auto sharedAlloc = std::make_shared<RegionAllocation>(bodyAlloc);

    // Reset loop-local energy state and apply the converged body allocation.
    std::set<SchematicBlock *> loopBlocks;
    for (llvm::BasicBlock *BB : L->blocks())
        loopBlocks.insert(graph.getOrCreate(BB));

    for (SchematicBlock *block : loopBlocks) {
        auto &meta = solution.blockMeta[block];
        meta.analyzed = true;
        meta.E_left = std::numeric_limits<double>::max();
        meta.E_to_leave = 0.0;

        auto &placements = solution.decidedPlacements[block];
        for (const auto &[gv, va] : bodyAlloc.vars)
            placements[gv] = va.placement;

        auto existingIt = solution.blockAllocation.find(block);
        if (existingIt != solution.blockAllocation.end())
            extendsAllocation(*existingIt->second, *sharedAlloc);
        else
            solution.blockAllocation[block] = sharedAlloc;
    }

    double energyLeftStart =
        params.capacity - params.E_pro - params.N_reg * params.regRestoreEnergy;
    for (const auto &[gv, va] : bodyAlloc.vars) {
        if (va.placement == Placement::VM)
            energyLeftStart -= params.memRestoreEnergyPerByte * state.getVarSizeBytes(gv);
    }

    double eToLeave =
        params.E_epi + params.N_reg * params.regStoreEnergy + params.loopIncrementCostNvm;
    for (const auto &[gv, va] : bodyAlloc.vars) {
        if (va.placement == Placement::VM)
            eToLeave += params.memStoreEnergyPerByte * state.getVarSizeBytes(gv);
    }

    auto isCurrentLoopBackedge = [&](SchematicBlock *src, SchematicBlock *dst) {
        return src == latchBlock && dst == headerBlock;
    };
    auto isBlockInLoop = [&](SchematicBlock *block) {
        llvm::BasicBlock *BB = block ? block->getLLVMBlock() : nullptr;
        return BB && L->contains(BB);
    };

    // Forward propagation over the real loop CFG (LoopBodyNoBackedge).
    // Like propagateEnergyLeft, this deliberately diverges from the reference's
    // greedy max-first walk (which can drop the worst-case path at joins whose
    // costlier side has more hops): build the DAG of reachable disabled edges
    // first, then propagate the max accumulated cost in topological order,
    // mirroring the backward pass below.
    {
        CFGEdge seedEdge{startSynth, headerBlock};
        std::deque<CFGEdge> toVisit = {seedEdge};
        std::set<CFGEdge> visited;
        std::map<CFGEdge, std::vector<CFGEdge>> dagAdj;
        dagAdj[seedEdge];

        while (!toVisit.empty()) {
            CFGEdge edge = toVisit.front();
            toVisit.pop_front();
            if (!visited.insert(edge).second)
                continue;

            SchematicBlock *block = edge.dst;
            if (!isBlockInLoop(block))
                continue;

            llvm::BasicBlock *BB = block->getLLVMBlock();
            for (llvm::BasicBlock *succBB : llvm::successors(BB)) {
                if (!L->contains(succBB))
                    continue;
                SchematicBlock *succ = graph.getOrCreate(succBB);
                CFGEdge childEdge{block, succ};
                if (visited.count(childEdge))
                    continue;
                if (isCurrentLoopBackedge(block, succ))
                    continue;
                if (!isDisabledCheckpoint(solution, childEdge))
                    continue;
                dagAdj[childEdge];
                dagAdj[edge].push_back(childEdge);
                toVisit.push_back(childEdge);
            }
        }

        std::map<CFGEdge, unsigned> inDeg;
        for (auto &[node, children] : dagAdj) {
            if (inDeg.find(node) == inDeg.end())
                inDeg[node] = 0;
            for (auto &child : children)
                inDeg[child]++;
        }

        std::deque<CFGEdge> topoQueue;
        for (auto &[node, deg] : inDeg)
            if (deg == 0)
                topoQueue.push_back(node);

        std::map<CFGEdge, double> maxCost;
        std::set<llvm::BasicBlock *> seenLoops;
        maxCost[seedEdge] = 0.0;

        while (!topoQueue.empty()) {
            CFGEdge edge = topoQueue.front();
            topoQueue.pop_front();

            double cost = maxCost.count(edge) ? maxCost[edge] : 0.0;
            maxCost.erase(edge);

            SchematicBlock *block = edge.dst;
            if (isBlockInLoop(block)) {
                auto metaIt = solution.blockMeta.find(block);
                if (metaIt != solution.blockMeta.end()) {
                    const auto &loopOpt = metaIt->second.loop;
                    if (loopOpt.has_value()) {
                        llvm::BasicBlock *loopHeader = loopOpt->loop->getHeader();
                        if (!seenLoops.count(loopHeader)) {
                            seenLoops.insert(loopHeader);
                            cost += (loopOpt->nbIter - 1) * loopOpt->costOneIt;
                        }
                    }
                }

                cost += getBlockExecEnergy(block, solution, cfg, state, params);
                double energyLeft = energyLeftStart - cost;
                if (energyLeft < solution.blockMeta[block].E_left)
                    solution.blockMeta[block].E_left = energyLeft;

                for (auto &childEdge : dagAdj[edge]) {
                    if (maxCost.find(childEdge) == maxCost.end() || cost > maxCost[childEdge])
                        maxCost[childEdge] = cost;
                }
            }

            for (auto &childEdge : dagAdj[edge]) {
                inDeg[childEdge]--;
                if (inDeg[childEdge] == 0)
                    topoQueue.push_back(childEdge);
            }
        }
    }

    // Backward propagation over the real loop CFG (LoopBodyNoBackedge).
    {
        std::deque<CFGEdge> toVisit = {CFGEdge{latchBlock, endSynth}};
        std::set<CFGEdge> visited;
        std::map<CFGEdge, std::vector<CFGEdge>> dagAdj;
        dagAdj[CFGEdge{latchBlock, endSynth}];

        while (!toVisit.empty()) {
            CFGEdge edge = toVisit.front();
            toVisit.pop_front();
            if (!visited.insert(edge).second)
                continue;

            SchematicBlock *block = edge.src;
            if (!isBlockInLoop(block))
                continue;

            llvm::BasicBlock *BB = block->getLLVMBlock();
            for (llvm::BasicBlock *predBB : llvm::predecessors(BB)) {
                if (!L->contains(predBB))
                    continue;
                SchematicBlock *pred = graph.getOrCreate(predBB);
                CFGEdge childEdge{pred, block};
                if (visited.count(childEdge))
                    continue;
                if (isCurrentLoopBackedge(pred, block))
                    continue;
                if (!isDisabledCheckpoint(solution, childEdge))
                    continue;
                dagAdj[childEdge];
                dagAdj[edge].push_back(childEdge);
                toVisit.push_back(childEdge);
            }
        }

        std::map<CFGEdge, unsigned> inDeg;
        for (auto &[node, children] : dagAdj) {
            if (inDeg.find(node) == inDeg.end())
                inDeg[node] = 0;
            for (auto &child : children)
                inDeg[child]++;
        }

        std::deque<CFGEdge> topoQueue;
        for (auto &[node, deg] : inDeg)
            if (deg == 0)
                topoQueue.push_back(node);

        std::map<CFGEdge, double> maxEToLeave;
        std::set<llvm::BasicBlock *> seenLoops;
        CFGEdge seedEdge{latchBlock, endSynth};
        maxEToLeave[seedEdge] = eToLeave;

        while (!topoQueue.empty()) {
            CFGEdge edge = topoQueue.front();
            topoQueue.pop_front();

            double energy = maxEToLeave.count(edge) ? maxEToLeave[edge] : 0.0;
            maxEToLeave.erase(edge);

            SchematicBlock *block = edge.src;
            if (isBlockInLoop(block)) {
                auto metaIt = solution.blockMeta.find(block);
                if (metaIt != solution.blockMeta.end()) {
                    const auto &loopOpt = metaIt->second.loop;
                    if (loopOpt.has_value()) {
                        llvm::BasicBlock *loopHeader = loopOpt->loop->getHeader();
                        if (!seenLoops.count(loopHeader)) {
                            seenLoops.insert(loopHeader);
                            energy += (loopOpt->nbIter - 1) * loopOpt->costOneIt;
                        }
                    }
                }

                energy += getBlockExecEnergy(block, solution, cfg, state, params);
                if (energy > solution.blockMeta[block].E_to_leave)
                    solution.blockMeta[block].E_to_leave = energy;
            }

            for (auto &childEdge : dagAdj[edge]) {
                if (maxEToLeave.find(childEdge) == maxEToLeave.end() ||
                    energy > maxEToLeave[childEdge]) {
                    maxEToLeave[childEdge] = energy;
                }
            }

            for (auto &childEdge : dagAdj[edge]) {
                inDeg[childEdge]--;
                if (inDeg[childEdge] == 0)
                    topoQueue.push_back(childEdge);
            }
        }
    }

    solution.blockMeta[startSynth].E_left = energyLeftStart;
    solution.blockMeta[startSynth].E_to_leave = solution.blockMeta[headerBlock].E_to_leave;
    if (latchBlock)
        solution.blockMeta[endSynth].E_left = solution.blockMeta[latchBlock].E_left;
    solution.blockMeta[endSynth].E_to_leave = eToLeave;

    auto hdPlIt = solution.decidedPlacements.find(headerBlock);
    if (hdPlIt != solution.decidedPlacements.end()) {
        solution.decidedPlacements[startSynth] = hdPlIt->second;
        solution.decidedPlacements[endSynth] = hdPlIt->second;
    }
}

static std::map<llvm::Value *, VariableAccessEstimate>
estimateLoopVariableAccess(const LoadedLoopTrace &loopTrace, const SchematicStateAnalysis &state,
                           uint64_t numIterations) {
    std::map<llvm::Value *, double> expectedAccesses;
    std::map<llvm::Value *, bool> needRestoreByVar;

    uint64_t totalExecutions = 0;
    for (const auto &path : loopTrace.iterationPaths)
        totalExecutions += path.count;

    double fallbackFreq = loopTrace.iterationPaths.empty()
                              ? 0.0
                              : 1.0 / static_cast<double>(loopTrace.iterationPaths.size());
    for (const auto &path : loopTrace.iterationPaths) {
        double freq = totalExecutions == 0
                          ? fallbackFreq
                          : static_cast<double>(path.count) / static_cast<double>(totalExecutions);
        for (SchematicBlock *block : path.blocks) {
            llvm::BasicBlock *BB = block->getLLVMBlock();
            if (!BB)
                continue;
            for (llvm::Value *v : state.getCandidates()) {
                unsigned accessCount = state.getLoadCount(BB, v) + state.getStoreCount(BB, v);
                if (accessCount == 0)
                    continue;
                expectedAccesses[v] +=
                    static_cast<double>(accessCount) * freq * static_cast<double>(numIterations);
                if (!needRestoreByVar.count(v)) {
                    auto firstOp = state.getFirstOpIsLoad(BB, v);
                    if (firstOp.has_value())
                        needRestoreByVar[v] = *firstOp;
                }
            }
        }
    }

    std::map<llvm::Value *, VariableAccessEstimate> result;
    for (const auto &[v, weightedCount] : expectedAccesses) {
        result[v] = {std::max<unsigned>(1, static_cast<unsigned>(std::llround(weightedCount))),
                     needRestoreByVar.count(v) ? needRestoreByVar[v] : false, true};
    }
    return result;
}

LoopAnalyzer::LoopAnalyzer(llvm::LoopInfo &LI, llvm::ScalarEvolution &SE, const CFGAnalysis &cfg,
                           const SchematicStateAnalysis &state, const SchematicParams &params,
                           VMAddressTracker *tracker, SchematicGraph &graph)
    : LI_(LI), SE_(SE), cfg_(cfg), state_(state), params_(params), tracker_(tracker),
      graph_(graph) {}

void LoopAnalyzer::setLoadedLoopTraces(const std::vector<LoadedLoopTrace> &traces) {
    loadedLoopTraces_ = traces;
}

std::optional<uint64_t> LoopAnalyzer::getMaxTripCount(llvm::Loop *L) const {
    // Try marker-based trip count first (from TripCountAnnotationPass).
    if (auto tc = getMarkerTripCount(L))
        return tc;
    // Fallback to ScalarEvolution.
    unsigned tc = SE_.getSmallConstantMaxTripCount(L);
    if (tc > 0)
        return static_cast<uint64_t>(tc);
    return std::nullopt;
}

RegionAllocation
LoopAnalyzer::buildBoundaryAllocation(const std::map<llvm::Value *, Placement> &placement) const {
    RegionAllocation alloc;
    alloc.vmBytesUsed = 0;
    for (const auto &[gv, place] : placement) {
        alloc.vars[gv].placement = place;
        if (place == Placement::VM) {
            alloc.vmOffsets[gv] = alloc.vmBytesUsed;
            alloc.vmBytesUsed += state_.getVarSizeBytes(gv);
        }
    }
    return alloc;
}

bool LoopAnalyzer::analyzeLoop(llvm::Loop *L, SchematicSolution &solution) {
    LoopBoundaryBlocks blocks = createLoopBoundaryBlocks(L, graph_);

    // Cleanup: erase synthetic block entries from solution maps on all exit paths.
    // Graph owns the synthetic blocks, so no delete needed.
    auto cleanup = [&]() { eraseSyntheticLoopBoundaryState(solution, blocks); };
    struct ScopeGuard {
        std::function<void()> fn;
        ~ScopeGuard() { fn(); }
    } guard{cleanup};

    // Get max trip count.
    auto tcOpt = getMaxTripCount(L);
    if (!tcOpt) {
        PLOGE << "SCHEMATIC: loop at " << blocks.header->getName()
              << " has no trip count annotation — cannot analyze";
        return false;
    }
    uint64_t maxTripCount = *tcOpt;

    // Get loop body paths (header-to-latch).
    const LoadedLoopTrace *matchedLoopTrace =
        findLoadedLoopTraceForHeader(blocks.headerBlock, loadedLoopTraces_);
    std::vector<std::vector<SchematicBlock *>> bodyPaths =
        matchedLoopTrace ? collectLoopBodyPaths(*matchedLoopTrace)
                         : std::vector<std::vector<SchematicBlock *>>{};
    if (bodyPaths.empty()) {
        PLOGE << "SCHEMATIC: loop at " << blocks.header->getName()
              << " has no analyzable body paths";
        return false;
    }

    // Run RCG solver on each body path.
    initializeSyntheticLoopBoundaryState(solution, state_, params_, blocks);
    std::string errorMsg;
    if (!analyzeLoopBodyPaths(bodyPaths, blocks, solution, state_, cfg_, params_, tracker_, LI_, L,
                              graph_, errorMsg)) {
        PLOGE << "SCHEMATIC infeasible: energy capacity too small for loop at '"
              << blocks.header->getName() << "': " << errorMsg;
        return false;
    }

    // Analyze uncovered blocks within this loop (reference: schematic.py:554).
    errorMsg.clear();
    if (!findAndAnalyzeNotFixedPaths(cfg_, solution, state_, params_, tracker_, LI_, L, graph_,
                                     errorMsg)) {
        PLOGE << "SCHEMATIC infeasible: uncovered block in loop at '" << blocks.header->getName()
              << "': " << errorMsg;
        return false;
    }

    // Resolve loop-internal edges (reference: schematic.py:555).
    removePotentialCheckpointsBetweenFixedBBs(cfg_, solution, state_, params_, LI_, graph_, L);
    refreshDisabledCheckpointEnergy(cfg_, solution, state_, params_, LI_, graph_, L);

    // Reconcile header and latch allocations.
    LoopBoundaryPlacements placements = resolveLoopBoundaryPlacements(solution, blocks);

    LoopCheckpointDecision decision;
    decision.loop = L;
    decision.bodyPathCount = static_cast<unsigned>(bodyPaths.size());

    if (placements.allocationsDiffer) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        decision.E_loop = 0.0;
        decision.bodyAllocation = buildBoundaryAllocation(placements.headerAlloc);
        solution.loopDecisions[blocks.headerBlock] = decision;
        propagateMandatoryLoopBoundary(L, blocks, decision.bodyAllocation, solution, cfg_, state_,
                                       params_, LI_);
        if (blocks.latchBlock)
            enableCheckpoint(solution, CFGEdge{blocks.latchBlock, blocks.headerBlock},
                             loopOriginTag(L, "alloc-mismatch-backedge"));
        return true;
    }

    // Compute E_loop and nb_it_with_budget.
    // E_loop = START_Loop.E_to_leave - END_Loop.E_to_leave + loop_increment_cost_nvm
    // (reference schematic.py:566-569, using synthetic boundary nodes).
    RegionAllocation bodyAlloc = buildBoundaryAllocation(placements.headerAlloc);

    // Copy energy values from header/latch to synthetic boundary blocks.
    // In Python, the synthetic blocks are connected in the networkx graph, so
    // apply_memory_allocation's propagation reaches them directly. In C++, the
    // synthetic blocks now have proper edges via SchematicGraph.
    // Reference: Python's first_bb/last_bb in E_loop computation (schematic.py:566-569).
    copyLoopBoundaryEnergyToSyntheticBlocks(solution, blocks);
    LoopIterationBudget budget = readLoopIterationBudget(blocks, params_, solution);
    decision.initialStartEToLeave = budget.startEToLeave;
    decision.initialEndEToLeave = budget.endEToLeave;
    decision.initialELoop = budget.ELoop;

    // Inner loop multi-iteration costs are handled by applyLoopIterationAdjustment's
    // direct E_to_leave/E_left adjustment on loop blocks (matching Python reference
    // schematic.py:643-664) and by propagation's seenLoops scaling (which
    // applies unconditionally, without loopScope filtering).

    decision.E_loop = budget.ELoop;
    decision.bodyAllocation = bodyAlloc;

    PLOGD << "[LoopAnalyzer] loop=" << blocks.header->getName() << " E_loop=" << budget.ELoop
          << " startEToLeave=" << budget.startEToLeave << " endEToLeave=" << budget.endEToLeave
          << " capacity=" << params_.capacity << " maxTripCount=" << maxTripCount;

    if (budget.ELoop <= 0.0) {
        decision.numIterationsPerCharge = 0;
        solution.loopDecisions[blocks.headerBlock] = decision;
        return true;
    }

    decision.initialAvailableEnergy = budget.availableEnergy;
    if (budget.availableEnergy <= 0.0) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        decision.finalStartEToLeave = budget.startEToLeave;
        decision.finalEndEToLeave = budget.endEToLeave;
        decision.finalAvailableEnergy = budget.availableEnergy;
        solution.loopDecisions[blocks.headerBlock] = decision;
        if (blocks.latchBlock)
            enableCheckpoint(solution, CFGEdge{blocks.latchBlock, blocks.headerBlock},
                             loopOriginTag(L, "nonpositive-available-energy"));
        return true;
    }

    decision.initialRawNumIterations = budget.rawNumIterations;

    // Convergence loop (reference lines 574-617).
    refineLoopBudgetWithConvergence(L, *matchedLoopTrace, maxTripCount, blocks, solution, cfg_,
                                    state_, params_, LI_, tracker_, graph_, budget, bodyAlloc,
                                    decision);

    decision.finalStartEToLeave = budget.startEToLeave;
    decision.finalEndEToLeave = budget.endEToLeave;
    decision.finalAvailableEnergy = budget.availableEnergy;
    decision.finalRawNumIterations = budget.rawNumIterations;

    // Decide checkpoint type.
    PLOGD << "[LoopAnalyzer] loop=" << blocks.header->getName() << " numIt=" << budget.numIterations
          << " maxTripCount=" << maxTripCount << " availableEnergy=" << budget.availableEnergy;
    applyLoopCheckpointPolicy(L, static_cast<unsigned>(maxTripCount), blocks, budget, solution,
                              decision);

    solution.loopDecisions[blocks.headerBlock] = decision;

    // Set LoopMark on blocks and adjust energy (reference: schematic.py:643-664).
    applyLoopIterationAdjustment(L, static_cast<unsigned>(maxTripCount), decision, budget.ELoop,
                                 solution, graph_);

    return true;
}

bool LoopAnalyzer::analyzeLoops(SchematicSolution &solution) {
    std::vector<std::string> coverageErrors =
        collectLoopTraceCoverageErrors(LI_, graph_, loadedLoopTraces_);
    if (!coverageErrors.empty()) {
        std::string message =
            "SCHEMATIC: incomplete loop trace coverage\n" + joinLines(coverageErrors);
        llvm::report_fatal_error(llvm::StringRef(message));
    }

    // Process only loops that have trace data, sorted by depth (innermost first).
    // Reference: schematic.py:675-684 — iterates over f_traces.loop_traces.values()
    // sorted by depth descending, NOT over all loops in the IR.
    // Loops not executed at runtime have no trace and are implicitly skipped.
    std::vector<LoadedLoopTrace> sorted = loadedLoopTraces_;
    std::sort(sorted.begin(), sorted.end(), [](const LoadedLoopTrace &a, const LoadedLoopTrace &b) {
        return a.depth > b.depth; // innermost first
    });
    for (const auto &lt : sorted) {
        if (!lt.loop)
            continue;
        if (!analyzeLoop(lt.loop, solution))
            return false;
    }

    return true;
}

} // namespace checkpoint
