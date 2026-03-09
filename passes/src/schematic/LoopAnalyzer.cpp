#include "schematic/LoopAnalyzer.h"
#include "common/LoopTripCount.h"
#include "schematic/RCGSolver.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/raw_ostream.h"

#include <cmath>
#include <set>

namespace checkpoint {

LoopAnalyzer::LoopAnalyzer(llvm::LoopInfo &LI, llvm::ScalarEvolution &SE, const CFGAnalysis &cfg,
                           const SchematicStateAnalysis &state, const SchematicParams &params,
                           VMAddressTracker *tracker)
    : LI_(LI), SE_(SE), cfg_(cfg), state_(state), params_(params), tracker_(tracker) {}

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

std::vector<std::vector<llvm::BasicBlock *>>
LoopAnalyzer::enumerateLoopPathsWithoutBackEdges(llvm::Loop *L) const {
    llvm::BasicBlock *header = L->getHeader();
    std::vector<llvm::BasicBlock *> loopBlocks = L->getBlocksVector();
    std::set<llvm::BasicBlock *> loopBlockSet(loopBlocks.begin(), loopBlocks.end());

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

    // Self-latching (single-block) loops must still be analyzed as one-iteration
    // paths. The single iteration path is [header].
    if (latches.count(header))
        result.push_back({header});

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

bool LoopAnalyzer::placementsDiffer(const std::map<llvm::Value *, Placement> &a,
                                    const std::map<llvm::Value *, Placement> &b) const {
    // Check all keys in union.
    std::set<llvm::Value *> allKeys;
    for (const auto &[k, _] : a)
        allKeys.insert(k);
    for (const auto &[k, _] : b)
        allKeys.insert(k);

    for (llvm::Value *v : allKeys) {
        auto itA = a.find(v);
        auto itB = b.find(v);
        Placement pA = (itA != a.end()) ? itA->second : Placement::NVM;
        Placement pB = (itB != b.end()) ? itB->second : Placement::NVM;
        if (pA != pB)
            return true;
    }
    return false;
}

RegionAllocation
LoopAnalyzer::buildBoundaryAllocation(const std::map<llvm::Value *, Placement> &placement) const {
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
            for (const auto &ep : lt.iterationPaths) {
                if (!ep.blocks.empty())
                    bodyPaths.push_back(ep.blocks);
            }
            usedTraces = !bodyPaths.empty();
            break;
        }
    }
    if (!usedTraces)
        bodyPaths = enumerateLoopPathsWithoutBackEdges(L);

    if (bodyPaths.empty()) {
        llvm::errs() << "SCHEMATIC: loop at " << header->getName()
                     << " has no analyzable body paths\n";
        return false;
    }

    // 3. Run RCG solver on each path.
    for (const auto &path : bodyPaths) {
        RCGSolver solver(path, state_, cfg_, params_, solution.blockMeta,
                         solution.decidedPlacements, nullptr, nullptr, tracker_);
        RCGResult result = solver.solve();
        if (!result.feasible) {
            llvm::errs() << "SCHEMATIC infeasible: energy capacity too small for loop at '"
                         << header->getName() << "': " << result.errorMessage << "\n";
            return false;
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
    std::map<llvm::Value *, Placement> headerAlloc;
    auto hdIt = solution.decidedPlacements.find(header);
    if (hdIt != solution.decidedPlacements.end())
        headerAlloc = hdIt->second;

    llvm::BasicBlock *latch = L->getLoopLatch();
    std::map<llvm::Value *, Placement> latchAlloc;
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

    // 6. Allocations match — compute energy per iteration using reference formula:
    //    E_loop = header.E_to_leave - latch.E_to_leave + loop_increment_cost_nvm
    RegionAllocation bodyAlloc = buildBoundaryAllocation(headerAlloc);
    double headerEToLeave = 0.0;
    double latchEToLeave = 0.0;
    {
        auto hMeta = solution.blockMeta.find(header);
        if (hMeta != solution.blockMeta.end())
            headerEToLeave = hMeta->second.E_to_leave;
        if (latch) {
            auto lMeta = solution.blockMeta.find(latch);
            if (lMeta != solution.blockMeta.end())
                latchEToLeave = lMeta->second.E_to_leave;
        }
    }
    double E_loop = headerEToLeave - latchEToLeave + params_.loopIncrementCostNvm;
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
        E_ckpt += params_.memStoreEnergyPerByte * size + params_.memRestoreEnergyPerByte * size;
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

    auto numIt = static_cast<unsigned>(std::floor(availableEnergy / E_loop));

    // Convergence loop (reference lines 574-617): if loop has only disabled
    // checkpoints and numIt > 1, re-estimate variable accesses scaled by
    // min(numIt, maxTripCount) iterations and re-compute allocation until
    // numIt converges.
    bool hasEnabledCheckpoints = false;
    for (const auto &ckpt : solution.enabledCheckpoints) {
        // Check if any enabled checkpoint is a back-edge of this loop.
        if (ckpt.dst == header && L->contains(ckpt.src)) {
            hasEnabledCheckpoints = true;
            break;
        }
    }

    if (!hasEnabledCheckpoints && numIt > 1) {
        for (unsigned iter = 0; iter < 15; ++iter) {
            unsigned prevNumIt = numIt;

            // Re-compute checkpoint overhead and available energy.
            E_ckpt = params_.E_pro + params_.E_epi +
                     params_.N_reg * (params_.regStoreEnergy + params_.regRestoreEnergy);
            for (const auto &[gv, place] : bodyAlloc.placement) {
                if (place != Placement::VM)
                    continue;
                unsigned size = state_.getVarSizeBytes(gv);
                E_ckpt +=
                    params_.memStoreEnergyPerByte * size + params_.memRestoreEnergyPerByte * size;
            }

            availableEnergy = params_.capacity - E_ckpt;
            if (availableEnergy <= 0.0)
                break;

            numIt = static_cast<unsigned>(std::floor(availableEnergy / E_loop));
            if (numIt == prevNumIt)
                break; // Converged.
        }
    }

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
