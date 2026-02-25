#include "schematic/LoopAnalyzer.h"
#include "common/LoopTripCount.h"
#include "schematic/RCGSolver.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <cmath>
#include <set>

namespace checkpoint {

LoopAnalyzer::LoopAnalyzer(llvm::LoopInfo &LI,
                           llvm::ScalarEvolution &SE,
                           const CFGAnalysis &cfg,
                           const StateAnalysis &state,
                           const SchematicParams &params)
    : LI_(LI), SE_(SE), cfg_(cfg), state_(state), params_(params) {}

void LoopAnalyzer::setLoadedLoopTraces(
    const std::vector<LoadedLoopTrace> &traces) {
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

std::vector<std::vector<llvm::BasicBlock *>>
LoopAnalyzer::enumerateLoopPathsWithoutBackEdges(llvm::Loop *L) const {
    llvm::BasicBlock *header = L->getHeader();
    std::vector<llvm::BasicBlock *> loopBlocks = L->getBlocksVector();
    std::set<llvm::BasicBlock *> loopBlockSet(loopBlocks.begin(),
                                               loopBlocks.end());

    // Identify latch blocks.
    std::set<llvm::BasicBlock *> latches;
    if (llvm::BasicBlock *latch = L->getLoopLatch()) {
        latches.insert(latch);
    } else {
        for (llvm::BasicBlock *pred : predecessors(header)) {
            if (L->contains(pred))
                latches.insert(pred);
        }
    }

    // DFS from header to latch blocks, skipping back-edges and inner loop blocks.
    std::vector<std::vector<llvm::BasicBlock *>> result;

    struct DFSState {
        llvm::BasicBlock *BB;
        std::vector<llvm::BasicBlock *> path;
    };

    std::vector<DFSState> stack;
    stack.push_back({header, {header}});

    while (!stack.empty()) {
        auto [BB, path] = std::move(stack.back());
        stack.pop_back();

        // If this is a latch, record the path.
        if (BB != header && latches.count(BB)) {
            result.push_back(std::move(path));
            continue;
        }

        for (llvm::BasicBlock *succ : successors(BB)) {
            // Skip back-edges to header.
            if (succ == header)
                continue;
            // Only visit blocks within this loop.
            if (!loopBlockSet.count(succ))
                continue;
            // Skip blocks belonging to inner loops (except their headers,
            // which act as summary nodes).
            llvm::Loop *succLoop = LI_.getLoopFor(succ);
            if (succLoop && succLoop != L && succ != succLoop->getHeader())
                continue;

            auto newPath = path;
            newPath.push_back(succ);
            stack.push_back({succ, std::move(newPath)});
        }
    }

    return result;
}

bool LoopAnalyzer::placementsDiffer(
    const std::map<llvm::GlobalVariable *, Placement> &a,
    const std::map<llvm::GlobalVariable *, Placement> &b) const {
    // Check all keys in union.
    std::set<llvm::GlobalVariable *> allKeys;
    for (const auto &[k, _] : a) allKeys.insert(k);
    for (const auto &[k, _] : b) allKeys.insert(k);

    for (llvm::GlobalVariable *gv : allKeys) {
        auto itA = a.find(gv);
        auto itB = b.find(gv);
        Placement pA = (itA != a.end()) ? itA->second : Placement::NVM;
        Placement pB = (itB != b.end()) ? itB->second : Placement::NVM;
        if (pA != pB)
            return true;
    }
    return false;
}

RegionAllocation LoopAnalyzer::buildBoundaryAllocation(
    const std::map<llvm::GlobalVariable *, Placement> &placement) const {
    RegionAllocation alloc;
    alloc.placement = placement;
    alloc.vmBytesUsed = 0;
    for (const auto &[gv, place] : placement) {
        if (place == Placement::VM) {
            alloc.vmOffsets[gv] = alloc.vmBytesUsed;
            alloc.vmBytesUsed += state_.getVarSizeBytes(gv);
        }
    }
    return alloc;
}

double LoopAnalyzer::computeMaxIterationEnergy(
    llvm::Loop *L, const RegionAllocation &allocation,
    const SchematicSolution &solution) const {

    llvm::BasicBlock *header = L->getHeader();
    std::vector<llvm::BasicBlock *> loopBlocks = L->getBlocksVector();
    std::set<llvm::BasicBlock *> loopBlockSet(loopBlocks.begin(),
                                               loopBlocks.end());

    // Build DAG of direct children blocks (only this nesting level).
    // Assign per-block energy.
    llvm::DenseMap<llvm::BasicBlock *, double> blockEnergy;

    for (llvm::BasicBlock *BB : loopBlocks) {
        llvm::Loop *innerLoop = LI_.getLoopFor(BB);

        // Skip blocks in inner loops that aren't inner loop headers.
        if (innerLoop && innerLoop != L && BB != innerLoop->getHeader())
            continue;

        if (innerLoop && innerLoop != L && BB == innerLoop->getHeader()) {
            // Inner loop header — check if it has a checkpoint decision.
            auto decIt = solution.loopDecisions.find(BB);
            if (decIt != solution.loopDecisions.end()) {
                const auto &dec = decIt->second;
                if (dec.numIterationsPerCharge == 0 && !dec.mandatoryBackEdge) {
                    // No checkpoint: entire inner loop fits in one charge.
                    // Use maxTripCount * E_loop.
                    auto tc = getMaxTripCount(innerLoop);
                    double tripCount = tc ? static_cast<double>(*tc) : 1.0;
                    blockEnergy[BB] = tripCount * dec.E_loop;
                } else {
                    // Has checkpoint: bounded by one charge cycle.
                    blockEnergy[BB] = params_.capacity;
                }
            } else {
                // No decision yet — use base energy cost.
                blockEnergy[BB] = cfg_.getBlockInfo(BB).energyCost;
            }
        } else {
            // Normal block: base cost minus NVM savings for VM-placed vars.
            double E = cfg_.getBlockInfo(BB).energyCost;
            for (const auto &[gv, place] : allocation.placement) {
                if (place != Placement::VM)
                    continue;
                unsigned loads = state_.getLoadCount(BB, gv);
                unsigned stores = state_.getStoreCount(BB, gv);
                E -= params_.nvmAccessPenalty * (loads + stores);
            }
            blockEnergy[BB] = E;
        }
    }

    // Identify latch blocks.
    std::set<llvm::BasicBlock *> latches;
    if (llvm::BasicBlock *latch = L->getLoopLatch()) {
        latches.insert(latch);
    } else {
        for (llvm::BasicBlock *pred : predecessors(header)) {
            if (L->contains(pred))
                latches.insert(pred);
        }
    }

    // Longest path via topological order on the loop body DAG.
    // Topological sort via Kahn's algorithm on the restricted set.
    std::set<llvm::BasicBlock *> dagBlocks;
    for (const auto &[BB, _] : blockEnergy)
        dagBlocks.insert(BB);

    llvm::DenseMap<llvm::BasicBlock *, unsigned> inDegree;
    llvm::DenseMap<llvm::BasicBlock *, llvm::SmallVector<llvm::BasicBlock *, 4>>
        dagSucc;

    for (llvm::BasicBlock *BB : dagBlocks) {
        inDegree[BB] = 0;
    }

    for (llvm::BasicBlock *BB : dagBlocks) {
        for (llvm::BasicBlock *succ : successors(BB)) {
            // Skip back-edges.
            if (succ == header)
                continue;
            // Map inner loop blocks to their header.
            llvm::BasicBlock *target = succ;
            if (dagBlocks.count(succ) == 0) {
                llvm::Loop *succLoop = LI_.getLoopFor(succ);
                if (succLoop && succLoop != L)
                    target = succLoop->getHeader();
            }
            if (dagBlocks.count(target) && target != BB) {
                dagSucc[BB].push_back(target);
                inDegree[target]++;
            }
        }
    }

    // Kahn's topological sort.
    std::vector<llvm::BasicBlock *> topoOrder;
    std::vector<llvm::BasicBlock *> queue;
    for (llvm::BasicBlock *BB : dagBlocks) {
        if (inDegree[BB] == 0)
            queue.push_back(BB);
    }

    while (!queue.empty()) {
        llvm::BasicBlock *BB = queue.back();
        queue.pop_back();
        topoOrder.push_back(BB);
        for (llvm::BasicBlock *succ : dagSucc[BB]) {
            if (--inDegree[succ] == 0)
                queue.push_back(succ);
        }
    }

    // Longest path from header.
    llvm::DenseMap<llvm::BasicBlock *, double> dist;
    for (llvm::BasicBlock *BB : dagBlocks)
        dist[BB] = -1.0;
    dist[header] = blockEnergy[header];

    for (llvm::BasicBlock *BB : topoOrder) {
        if (dist[BB] < 0.0)
            continue;
        for (llvm::BasicBlock *succ : dagSucc[BB]) {
            double newDist = dist[BB] + blockEnergy[succ];
            if (newDist > dist[succ])
                dist[succ] = newDist;
        }
    }

    // Maximum across all blocks in the DAG.
    double maxEnergy = 0.0;
    for (llvm::BasicBlock *BB : dagBlocks) {
        if (dist[BB] > maxEnergy)
            maxEnergy = dist[BB];
    }

    return maxEnergy;
}

bool LoopAnalyzer::analyzeLoop(llvm::Loop *L, SchematicSolution &solution) {
    llvm::BasicBlock *header = L->getHeader();

    // 1. Get max trip count.
    auto tcOpt = getMaxTripCount(L);
    if (!tcOpt) {
        llvm::errs() << "SCHEMATIC: loop at " << header->getName()
                      << " has no trip count annotation — cannot analyze\n";
        return false;
    }
    uint64_t maxTripCount = *tcOpt;

    // 2. Get loop body paths (header-to-latch).
    std::vector<std::vector<llvm::BasicBlock *>> bodyPaths;

    // Check if we have loaded traces for this loop.
    bool usedTraces = false;
    for (const auto &lt : loadedLoopTraces_) {
        if (lt.header == header) {
            for (const auto &ep : lt.iterationPaths)
                bodyPaths.push_back(ep.blocks);
            usedTraces = true;
            break;
        }
    }
    if (!usedTraces)
        bodyPaths = enumerateLoopPathsWithoutBackEdges(L);

    if (bodyPaths.empty()) {
        // Single-block loop or trivial — no analysis needed.
        LoopCheckpointDecision decision;
        decision.loop = L;
        decision.numIterationsPerCharge = 0;
        decision.E_loop = 0.0;
        solution.loopDecisions[header] = decision;
        return true;
    }

    // 3. Run RCG solver on each path.
    for (const auto &path : bodyPaths) {
        RCGSolver solver(path, state_, cfg_, params_,
                         solution.blockMeta, solution.decidedPlacements,
                         nullptr, nullptr);
        RCGResult result = solver.solve();
        if (!result.feasible) {
            llvm::report_fatal_error(
                llvm::Twine("SCHEMATIC infeasible loop body path at header '") +
                    header->getName() + "': " + result.errorMessage,
                /*GenCrashDiag=*/false);
        }

        // Update solution from RCG result.
        for (const auto &ckpt : result.selectedCheckpoints)
            solution.enabledCheckpoints.insert(ckpt);

        for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
            const auto &blocks = result.intervalBlocks[i];
            const auto &alloc = result.allocations[i];

            // Update decided placements and block metadata.
            for (llvm::BasicBlock *BB : blocks) {
                for (const auto &[gv, place] : alloc.placement)
                    solution.decidedPlacements[BB][gv] = place;

                auto &meta = solution.blockMeta[BB];
                meta.analyzed = true;
            }

            // Store as region.
            solution.regions.push_back({blocks, alloc});
        }
    }

    // 4. Get header and latch allocations from decided placements.
    std::map<llvm::GlobalVariable *, Placement> headerAlloc;
    auto hdIt = solution.decidedPlacements.find(header);
    if (hdIt != solution.decidedPlacements.end())
        headerAlloc = hdIt->second;

    llvm::BasicBlock *latch = L->getLoopLatch();
    std::map<llvm::GlobalVariable *, Placement> latchAlloc;
    if (latch) {
        auto ltIt = solution.decidedPlacements.find(latch);
        if (ltIt != solution.decidedPlacements.end())
            latchAlloc = ltIt->second;
    }

    LoopCheckpointDecision decision;
    decision.loop = L;

    // 5. Check if allocations differ at header vs latch.
    if (placementsDiffer(headerAlloc, latchAlloc)) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        decision.E_loop = 0.0;
        decision.bodyAllocation = buildBoundaryAllocation(headerAlloc);
        solution.loopDecisions[header] = decision;

        // Add back-edge checkpoint.
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        return true;
    }

    // 6. Allocations match — compute energy per iteration.
    RegionAllocation bodyAlloc = buildBoundaryAllocation(headerAlloc);
    double E_loop = computeMaxIterationEnergy(L, bodyAlloc, solution);
    decision.E_loop = E_loop;
    decision.bodyAllocation = bodyAlloc;

    // Checkpoint overhead.
    double E_ckpt = params_.E_pro + params_.E_epi +
                    params_.N_reg * (params_.regStoreEnergy + params_.regRestoreEnergy);
    // Add VM save/restore costs for the body allocation.
    for (const auto &[gv, place] : bodyAlloc.placement) {
        if (place != Placement::VM)
            continue;
        unsigned size = state_.getVarSizeBytes(gv);
        E_ckpt += params_.memStoreEnergyPerByte * size +
                  params_.memRestoreEnergyPerByte * size;
    }

    if (E_loop <= 0.0) {
        // Degenerate case.
        decision.numIterationsPerCharge = 0;
        solution.loopDecisions[header] = decision;
        return true;
    }

    double availableEnergy = params_.capacity - E_ckpt;
    if (availableEnergy <= 0.0) {
        // Can't even fit one checkpoint + one iteration.
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        solution.loopDecisions[header] = decision;
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        return true;
    }

    unsigned numIt = static_cast<unsigned>(std::floor(availableEnergy / E_loop));

    if (numIt >= maxTripCount) {
        // Entire loop fits — no checkpoint needed.
        decision.numIterationsPerCharge = 0;
    } else if (numIt < 3) {
        // Too few iterations per charge — checkpoint every iteration.
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
    } else {
        // Conditional checkpoint every numIt iterations.
        decision.numIterationsPerCharge = numIt;
    }

    solution.loopDecisions[header] = decision;
    return true;
}

bool LoopAnalyzer::analyzeLoops(SchematicSolution &solution) {
    // Process loops bottom-up (innermost first).
    auto loops = LI_.getLoopsInPreorder();
    for (auto it = loops.rbegin(); it != loops.rend(); ++it) {
        if (!analyzeLoop(*it, solution))
            return false;
    }
    return true;
}

} // namespace checkpoint
