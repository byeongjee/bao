#include "schematic/LoopAnalyzer.h"
#include "common/Logger.h"
#include "common/LoopTripCount.h"
#include "schematic/IntervalAllocator.h"
#include "schematic/RCGSolver.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"

#include <cmath>
#include <limits>
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
        PLOGE << "SCHEMATIC: loop at " << header->getName()
              << " has no trip count annotation — cannot analyze";
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
        PLOGE << "SCHEMATIC: loop at " << header->getName() << " has no analyzable body paths";
        return false;
    }

    // 3. Run RCG solver on each path.
    // Use header/latch as boundary blocks if they have existing placements.
    llvm::BasicBlock *loopLatch = L->getLoopLatch();
    llvm::BasicBlock *startBound = nullptr;
    llvm::BasicBlock *endBound = nullptr;
    if (solution.decidedPlacements.count(header))
        startBound = header;
    if (loopLatch && solution.decidedPlacements.count(loopLatch))
        endBound = loopLatch;

    BlockCostOverrides innerOverrides = computeInnerLoopCostOverrides(L, solution);
    const BlockCostOverrides *overridesPtr = innerOverrides.empty() ? nullptr : &innerOverrides;

    for (const auto &path : bodyPaths) {
        RCGSolver solver(path, state_, cfg_, params_, solution.blockMeta,
                         solution.decidedPlacements, startBound, endBound, tracker_, overridesPtr);
        RCGResult result = solver.solve();
        if (!result.feasible) {
            PLOGE << "SCHEMATIC infeasible: energy capacity too small for loop at '"
                  << header->getName() << "': " << result.errorMessage;
            return false;
        }

        // Update solution from RCG result.
        for (const auto &ckpt : result.selectedCheckpoints)
            solution.enabledCheckpoints.insert(ckpt);

        for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
            const auto &blocks = result.intervalBlocks[i];
            const auto &alloc = result.allocations[i];

            // Update decided placements and block metadata.
            // NOTE: E_left/E_to_leave are not computed here because the
            // energy propagation model (SchematicPass Step 9c) handles them
            // separately. Setting values here could interfere with the
            // fixed-point propagation loops.
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
        llvm::errs() << "LOOP_DEBUG: loop=" << header->getName()
                     << " -> mandatoryBackEdge (placementsDiffer)\n";
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        decision.E_loop = 0.0;
        decision.bodyAllocation = buildBoundaryAllocation(headerAlloc);
        solution.loopDecisions[header] = decision;

        // Propagate energy so outer loops see correct costs on loop blocks.
        propagateLoopEnergy(L, decision.bodyAllocation, solution);

        // Add back-edge checkpoint.
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        return true;
    }

    // DEBUG: Print body paths for loops with maxTripCount <= 5 (outer loops)
    if (maxTripCount <= 5) {
        llvm::errs() << "OUTER_LOOP_DEBUG: loop=" << header->getName() << " maxTC=" << maxTripCount
                     << " bodyPaths=" << bodyPaths.size() << "\n";
        for (unsigned p = 0; p < bodyPaths.size(); ++p) {
            llvm::errs() << "  path[" << p << "]: ";
            for (auto *BB : bodyPaths[p])
                llvm::errs() << BB->getName() << " ";
            llvm::errs() << "\n";
            // Print E_to_leave for each block
            for (auto *BB : bodyPaths[p]) {
                auto it = solution.blockMeta.find(BB);
                double etl = (it != solution.blockMeta.end()) ? it->second.E_to_leave : -1;
                llvm::errs() << "    " << BB->getName() << " E_to_leave=" << etl << "\n";
            }
        }
    }

    // 6. Allocations match — compute energy per iteration using reference formula:
    //    E_loop = header.E_to_leave - latch.E_to_leave + latchCost + loop_increment_cost_nvm
    //
    //    The reference uses synthetic zero-cost START_Loop/END_Loop boundary nodes,
    //    so first_bb.E_to_leave - last_bb.E_to_leave captures ALL block costs
    //    (header through latch inclusive). We use real blocks, so header.E_to_leave
    //    already includes latch's cost in the accumulator, meaning the difference
    //    misses the latch's own execution cost. We add it back explicitly.
    RegionAllocation bodyAlloc = buildBoundaryAllocation(headerAlloc);

    // Run initial energy propagation before computing E_loop — without this,
    // E_to_leave/E_left are at defaults (0.0 / max) since RCG does not set them.
    propagateLoopEnergy(L, bodyAlloc, solution);

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
    double latchCost = latch ? getAdjustedBlockEnergy(latch, bodyAlloc) : 0.0;
    double E_loop = headerEToLeave - latchEToLeave + latchCost + params_.loopIncrementCostNvm;
    decision.E_loop = E_loop;
    decision.bodyAllocation = bodyAlloc;

    if (E_loop <= 0.0) {
        // Degenerate case.
        decision.numIterationsPerCharge = 0;
        solution.loopDecisions[header] = decision;
        return true;
    }

    // Reference: nb_it = (budget - latch.E_to_leave) // energy_one_it - 1
    double availableEnergy = params_.capacity - latchEToLeave;
    if (availableEnergy <= 0.0) {
        // Can't fit one checkpoint + one iteration.
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        solution.loopDecisions[header] = decision;
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        return true;
    }

    int rawNumIt = static_cast<int>(std::floor(availableEnergy / E_loop)) - 1;
    auto numIt = static_cast<unsigned>(std::max(rawNumIt, 0));

    // Convergence loop (reference lines 574-617): if loop has only disabled
    // checkpoints and numIt > 1, re-estimate variable accesses scaled by
    // min(numIt, maxTripCount) iterations and re-compute allocation until
    // numIt converges.
    bool hasEnabledCheckpoints = false;
    for (const auto &ckpt : solution.enabledCheckpoints) {
        // Check if any enabled checkpoint is internal to this loop
        // (both src and dst within the loop). Reference checks all edges,
        // not just back-edges to the header.
        if (L->contains(ckpt.src) && L->contains(ckpt.dst)) {
            hasEnabledCheckpoints = true;
            break;
        }
    }

    // Convergence loop (reference lines 574-617): re-estimate variable accesses
    // scaled by min(numIt, maxTripCount), re-allocate, re-propagate energy,
    // and re-compute nb_it until convergence.
    if (!hasEnabledCheckpoints && numIt > 1) {
        std::vector<llvm::BasicBlock *> loopBlocks = L->getBlocksVector();
        unsigned oldNumIt = 0;
        for (unsigned iter = 0; iter < 15; ++iter) {
            if (oldNumIt != 0 && numIt >= oldNumIt)
                break; // Converged
            oldNumIt = numIt;

            // 1. Scale access counts by min(numIt, maxTripCount).
            unsigned scaledIters =
                static_cast<unsigned>(std::min(static_cast<uint64_t>(numIt), maxTripCount));

            // 2. Re-compute allocation with scaled accesses.
            std::map<llvm::Value *, Placement> fixed;
            auto hdIt2 = solution.decidedPlacements.find(header);
            if (hdIt2 != solution.decidedPlacements.end())
                fixed = hdIt2->second;
            RegionAllocation newAlloc = computeIntervalAllocation(
                loopBlocks, state_, params_, fixed, tracker_, nullptr, nullptr, scaledIters);
            bodyAlloc = newAlloc;

            // 3. Apply allocation to loop blocks.
            for (llvm::BasicBlock *BB : loopBlocks) {
                for (const auto &[gv, place] : bodyAlloc.placement)
                    solution.decidedPlacements[BB][gv] = place;
                solution.blockMeta[BB].analyzed = true;
            }

            // 4. Re-compute E_to_leave/E_left through body paths.
            propagateLoopEnergy(L, bodyAlloc, solution);

            // 5. Recompute E_loop (with latch cost correction).
            auto hMeta2 = solution.blockMeta.find(header);
            auto lMeta2 = latch ? solution.blockMeta.find(latch) : solution.blockMeta.end();
            headerEToLeave = (hMeta2 != solution.blockMeta.end()) ? hMeta2->second.E_to_leave : 0.0;
            latchEToLeave = (lMeta2 != solution.blockMeta.end()) ? lMeta2->second.E_to_leave : 0.0;
            latchCost = latch ? getAdjustedBlockEnergy(latch, bodyAlloc) : 0.0;
            E_loop = headerEToLeave - latchEToLeave + latchCost + params_.loopIncrementCostNvm;
            decision.E_loop = E_loop;
            decision.bodyAllocation = bodyAlloc;

            if (E_loop <= 0.0)
                break;

            // 6. Recompute nb_it.
            // Reference uses header.E_to_leave here (not latch as in the
            // initial computation) because after re-propagation the header's
            // E_to_leave reflects the updated allocation costs.
            availableEnergy = params_.capacity - headerEToLeave;
            if (availableEnergy <= 0.0)
                break;
            rawNumIt = static_cast<int>(std::floor(availableEnergy / E_loop)) - 1;
            numIt = static_cast<unsigned>(std::max(rawNumIt, 0));
        }
    }

    llvm::errs() << "LOOP_DEBUG: loop=" << header->getName() << " E_loop=" << E_loop
                 << " numIt=" << numIt << " maxTC=" << maxTripCount
                 << " headerEToLeave=" << headerEToLeave << " latchEToLeave=" << latchEToLeave
                 << " latchCost=" << latchCost << " hasEnabledCkpts=" << hasEnabledCheckpoints
                 << " availEnergy=" << availableEnergy << "\n";

    if (numIt > maxTripCount) {
        // Entire loop fits — no checkpoint needed, but use maxTripCount for
        // energy scaling so propagation accounts for all iterations.
        decision.numIterationsPerCharge = static_cast<unsigned>(maxTripCount);
        decision.loopFitsEntirely = true;
        llvm::errs() << "LOOP_DEBUG:   -> loopFitsEntirely (numIt=" << numIt
                     << " > maxTC=" << maxTripCount << ")\n";
    } else if (numIt < 3) {
        // Too few iterations per charge — checkpoint every iteration.
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        llvm::errs() << "LOOP_DEBUG:   -> mandatoryBackEdge (numIt=" << numIt << ")\n";
    } else {
        // Conditional checkpoint every numIt iterations.
        decision.numIterationsPerCharge = numIt;
        llvm::errs() << "LOOP_DEBUG:   -> conditional (numIt=" << numIt << ")\n";
    }

    solution.loopDecisions[header] = decision;

    // Adjust E_left/E_to_leave for loop blocks when multiple iterations per charge.
    // E_left may go negative after adjustment — this is expected and correctly
    // reflects that the block's remaining energy budget is consumed by subsequent
    // iterations. Downstream propagateEnergy() handles negative E_left correctly.
    if (decision.numIterationsPerCharge > 1) {
        unsigned adjIter = decision.numIterationsPerCharge;
        if (decision.loopFitsEntirely)
            adjIter = static_cast<unsigned>(maxTripCount);
        double adj = (adjIter - 1) * E_loop;
        for (llvm::BasicBlock *BB : L->getBlocksVector()) {
            auto &meta = solution.blockMeta[BB];
            meta.E_to_leave += adj;
            meta.E_left -= adj;
        }
    }

    return true;
}

double LoopAnalyzer::getAdjustedBlockEnergy(llvm::BasicBlock *BB,
                                            const RegionAllocation &alloc) const {
    double bbE = cfg_.getBlockInfo(BB).energyCost;
    for (const auto &[gv, place] : alloc.placement) {
        if (place != Placement::VM)
            continue;
        unsigned loads = state_.getLoadCount(BB, gv);
        unsigned stores = state_.getStoreCount(BB, gv);
        bbE -= params_.nvmAccessPenalty * (loads + stores);
    }
    return bbE;
}

void LoopAnalyzer::propagateLoopEnergy(llvm::Loop *L, const RegionAllocation &alloc,
                                       SchematicSolution &solution) {
    llvm::BasicBlock *header = L->getHeader();
    llvm::BasicBlock *latch = L->getLoopLatch();

    // Collect all blocks in this loop (including inner loop blocks).
    std::vector<llvm::BasicBlock *> loopBlocks = L->getBlocksVector();
    std::set<llvm::BasicBlock *> loopBlockSet(loopBlocks.begin(), loopBlocks.end());

    // --- Backward pass: propagate E_to_leave ---
    // Seed: checkpoint-save cost after the latch (reference schematic.py:604-606).
    double bwdSeed =
        params_.E_epi + params_.N_reg * params_.regStoreEnergy + params_.loopIncrementCostNvm;
    for (const auto &[gv, place] : alloc.placement) {
        if (place == Placement::VM)
            bwdSeed += params_.memStoreEnergyPerByte * state_.getVarSizeBytes(gv);
    }

    // Initialize latch E_to_leave = seed + latch execution cost.
    if (latch && loopBlockSet.count(latch)) {
        double latchCost = getAdjustedBlockEnergy(latch, alloc);
        double latchEToLeave = bwdSeed + latchCost;
        auto &meta = solution.blockMeta[latch];
        if (latchEToLeave > meta.E_to_leave)
            meta.E_to_leave = latchEToLeave;
    }

    // Seed E_to_leave at enabled checkpoint sources within the loop.
    for (const auto &ckpt : solution.enabledCheckpoints) {
        if (!loopBlockSet.count(ckpt.src) || !loopBlockSet.count(ckpt.dst))
            continue;
        if (ckpt.dst == header) // back-edge checkpoint handled separately
            continue;
        double saveE = bwdSeed; // reuse same save-cost seed
        saveE += getAdjustedBlockEnergy(ckpt.src, alloc);
        auto &meta = solution.blockMeta[ckpt.src];
        if (saveE > meta.E_to_leave)
            meta.E_to_leave = saveE;
    }

    // Fixed-point: for each edge (src->dst), src.E_to_leave = max(current, srcCost +
    // dst.E_to_leave). Skip enabled checkpoints, back-edges, edges leaving the loop.
    bool changed = true;
    while (changed) {
        changed = false;
        for (llvm::BasicBlock *srcBB : loopBlocks) {
            for (llvm::BasicBlock *dstBB : successors(srcBB)) {
                if (!loopBlockSet.count(dstBB))
                    continue;
                if (dstBB == header)
                    continue;
                CFGEdge edge{srcBB, dstBB};
                if (solution.enabledCheckpoints.count(edge))
                    continue;

                auto dstIt = solution.blockMeta.find(dstBB);
                if (dstIt == solution.blockMeta.end())
                    continue;

                double srcCost = getAdjustedBlockEnergy(srcBB, alloc);
                double newEToLeave = srcCost + dstIt->second.E_to_leave;
                auto &srcMeta = solution.blockMeta[srcBB];
                if (newEToLeave > srcMeta.E_to_leave) {
                    srcMeta.E_to_leave = newEToLeave;
                    changed = true;
                }
            }
        }
    }

    // --- Forward pass: propagate E_left ---
    // Seed: capacity - restore cost (reference schematic.py:599-603).
    double fwdSeed = params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
    for (const auto &[gv, place] : alloc.placement) {
        if (place == Placement::VM)
            fwdSeed -= params_.memRestoreEnergyPerByte * state_.getVarSizeBytes(gv);
    }

    // Initialize header E_left = fwdSeed - header execution cost.
    {
        double headerCost = getAdjustedBlockEnergy(header, alloc);
        double headerELeft = fwdSeed - headerCost;
        auto &meta = solution.blockMeta[header];
        if (headerELeft < meta.E_left)
            meta.E_left = headerELeft;
    }

    // Seed E_left at enabled checkpoint destinations within the loop.
    for (const auto &ckpt : solution.enabledCheckpoints) {
        if (!loopBlockSet.count(ckpt.src) || !loopBlockSet.count(ckpt.dst))
            continue;
        if (ckpt.dst == header)
            continue;
        double restoreE = params_.E_pro + params_.N_reg * params_.regRestoreEnergy;
        for (const auto &[gv, place] : alloc.placement) {
            if (place == Placement::VM)
                restoreE += params_.memRestoreEnergyPerByte * state_.getVarSizeBytes(gv);
        }
        double dstCost = getAdjustedBlockEnergy(ckpt.dst, alloc);
        double newELeft = params_.capacity - restoreE - dstCost;
        auto &meta = solution.blockMeta[ckpt.dst];
        if (newELeft < meta.E_left)
            meta.E_left = newELeft;
    }

    // Fixed-point: for each edge (src->dst), dst.E_left = min(current, src.E_left - dstCost).
    changed = true;
    while (changed) {
        changed = false;
        for (llvm::BasicBlock *srcBB : loopBlocks) {
            for (llvm::BasicBlock *dstBB : successors(srcBB)) {
                if (!loopBlockSet.count(dstBB))
                    continue;
                if (dstBB == header)
                    continue;
                CFGEdge edge{srcBB, dstBB};
                if (solution.enabledCheckpoints.count(edge))
                    continue;

                auto srcIt = solution.blockMeta.find(srcBB);
                if (srcIt == solution.blockMeta.end())
                    continue;

                double dstCost = getAdjustedBlockEnergy(dstBB, alloc);
                double newELeft = srcIt->second.E_left - dstCost;
                auto &dstMeta = solution.blockMeta[dstBB];
                if (newELeft < dstMeta.E_left) {
                    dstMeta.E_left = newELeft;
                    changed = true;
                }
            }
        }
    }
}

BlockCostOverrides
LoopAnalyzer::computeInnerLoopCostOverrides(llvm::Loop *L,
                                            const SchematicSolution &solution) const {
    BlockCostOverrides overrides;
    for (llvm::Loop *sub : L->getSubLoops()) {
        llvm::BasicBlock *header = sub->getHeader();
        auto decIt = solution.loopDecisions.find(header);
        if (decIt == solution.loopDecisions.end())
            continue;
        const auto &dec = decIt->second;
        if (dec.E_loop <= 0.0)
            continue;
        overrides[header] = dec.numIterationsPerCharge * dec.E_loop;
    }
    return overrides;
}

bool LoopAnalyzer::analyzeLoops(SchematicSolution &solution) {
    // Process loops bottom-up (innermost first).
    auto loops = LI_.getLoopsInPreorder();
    for (auto it = loops.rbegin(); it != loops.rend(); ++it) {
        if (!analyzeLoop(*it, solution))
            return false;
    }

    // Undo iteration-scaling adjustments and re-propagate at single-iteration
    // scale. The adjustments were needed during bottom-up analysis so outer
    // loops could see inner loop multi-iteration costs via the accumulator.
    // Now that all loop decisions are final, we reset to single-iteration
    // E_to_leave/E_left values. This prevents the function-level RCG solver
    // from seeing inflated E_to_leave on loop boundary blocks (which would
    // make budget = capacity - E_to_leave go negative).
    // SchematicPass::propagateEnergy() will apply iteration scaling correctly
    // via loopDecisions, producing the same final values as the reference.

    // Phase 1: Reset E_to_leave/E_left on all loop blocks to defaults.
    for (auto it = loops.rbegin(); it != loops.rend(); ++it) {
        llvm::Loop *L = *it;
        for (llvm::BasicBlock *BB : L->getBlocksVector()) {
            auto &meta = solution.blockMeta[BB];
            meta.E_to_leave = 0.0;
            meta.E_left = std::numeric_limits<double>::max();
        }
    }

    // Phase 2: Re-propagate energy bottom-up at single-iteration scale.
    for (auto it = loops.rbegin(); it != loops.rend(); ++it) {
        llvm::Loop *L = *it;
        llvm::BasicBlock *header = L->getHeader();
        auto decIt = solution.loopDecisions.find(header);
        if (decIt == solution.loopDecisions.end())
            continue;

        propagateLoopEnergy(L, decIt->second.bodyAllocation, solution);
    }

    return true;
}

} // namespace checkpoint
