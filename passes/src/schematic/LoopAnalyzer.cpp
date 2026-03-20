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

    // Step 1: Get max trip count.
    auto tcOpt = getMaxTripCount(L);
    if (!tcOpt) {
        PLOGE << "SCHEMATIC: loop at " << header->getName()
              << " has no trip count annotation — cannot analyze";
        return false;
    }
    uint64_t maxTripCount = *tcOpt;

    // Step 2: Get loop body paths (header-to-latch).
    std::vector<std::vector<llvm::BasicBlock *>> bodyPaths;
    for (const auto &lt : loadedLoopTraces_) {
        if (lt.header == header) {
            for (const auto &ep : lt.iterationPaths) {
                if (!ep.blocks.empty())
                    bodyPaths.push_back(ep.blocks);
            }
            break;
        }
    }
    if (bodyPaths.empty()) {
        PLOGE << "SCHEMATIC: loop at " << header->getName() << " has no analyzable body paths";
        return false;
    }

    // Step 3: Run RCG solver on each body path.
    // No startBound/endBound — loops are analyzed independently (reference uses
    // synthetic START_Loop/END_Loop nodes with no prior placement constraints).
    // No costOverrides — RCG uses single-iteration block costs.
    for (const auto &path : bodyPaths) {
        RCGSolver solver(path, state_, cfg_, params_, solution.blockMeta,
                         solution.decidedPlacements,
                         /*startBoundaryBlock=*/nullptr, /*endBoundaryBlock=*/nullptr, tracker_);
        RCGResult result = solver.solve();
        if (!result.feasible) {
            PLOGE << "SCHEMATIC infeasible: energy capacity too small for loop at '"
                  << header->getName() << "': " << result.errorMessage;
            return false;
        }

        // Update solution from RCG result.
        for (const auto &ckpt : result.selectedCheckpoints)
            solution.enabledCheckpoints.insert(resolveCheckpointEdge(ckpt));

        for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
            const auto &blocks = result.intervalBlocks[i];
            const auto &alloc = result.allocations[i];
            for (llvm::BasicBlock *BB : blocks) {
                for (const auto &[gv, place] : alloc.placement)
                    solution.decidedPlacements[BB][gv] = place;
                solution.blockMeta[BB].analyzed = true;
            }
            solution.regions.push_back({blocks, alloc});
        }
    }

    // Step 3b: Analyze uncovered blocks within this loop (reference: schematic.py:554).
    // Blocks not on any body path (e.g., preheaders created by LoopSimplify)
    // must be analyzed here, within the loop context, before multi-iteration
    // scaling is applied. The reference calls find_and_analyse_not_fixed_paths
    // on the loop subgraph after analyzing loop traces.
    {
        // Seed E_left/E_to_leave so boundary blocks have values for RCG budget.
        std::map<llvm::Value *, Placement> headerPlacement;
        auto hdIt0 = solution.decidedPlacements.find(header);
        if (hdIt0 != solution.decidedPlacements.end())
            headerPlacement = hdIt0->second;
        RegionAllocation prelimAlloc = buildBoundaryAllocation(headerPlacement);
        propagateLoopEnergy(L, prelimAlloc, solution);

        std::vector<llvm::BasicBlock *> loopBlocks = L->getBlocksVector();
        for (llvm::BasicBlock *BB : loopBlocks) {
            auto metaIt = solution.blockMeta.find(BB);
            if (metaIt != solution.blockMeta.end() && metaIt->second.analyzed)
                continue;

            // Build synthetic path of contiguous unanalyzed blocks.
            std::vector<llvm::BasicBlock *> synPath;
            llvm::BasicBlock *sBound = nullptr;
            llvm::BasicBlock *eBound = nullptr;

            // Find an analyzed predecessor within the loop as start boundary.
            for (llvm::BasicBlock *pred : predecessors(BB)) {
                if (!L->contains(pred))
                    continue;
                auto pMeta = solution.blockMeta.find(pred);
                if (pMeta != solution.blockMeta.end() && pMeta->second.analyzed) {
                    sBound = pred;
                    break;
                }
            }

            // Walk forward through unanalyzed blocks within the loop.
            std::set<llvm::BasicBlock *> visited;
            std::vector<llvm::BasicBlock *> stack;
            stack.push_back(BB);
            while (!stack.empty()) {
                llvm::BasicBlock *cur = stack.back();
                stack.pop_back();
                if (visited.count(cur))
                    continue;
                if (!L->contains(cur))
                    continue;
                auto curMeta = solution.blockMeta.find(cur);
                if (curMeta != solution.blockMeta.end() && curMeta->second.analyzed)
                    continue;
                visited.insert(cur);
                synPath.push_back(cur);
                for (llvm::BasicBlock *succ : successors(cur)) {
                    if (!L->contains(succ))
                        continue;
                    auto sMeta = solution.blockMeta.find(succ);
                    if (sMeta == solution.blockMeta.end() || !sMeta->second.analyzed) {
                        stack.push_back(succ);
                        break; // greedy: follow one successor
                    }
                }
            }

            if (synPath.empty())
                continue;

            // Find an analyzed successor within the loop as end boundary.
            llvm::BasicBlock *lastBB = synPath.back();
            for (llvm::BasicBlock *succ : successors(lastBB)) {
                if (!L->contains(succ))
                    continue;
                auto sMeta = solution.blockMeta.find(succ);
                if (sMeta != solution.blockMeta.end() && sMeta->second.analyzed) {
                    eBound = succ;
                    break;
                }
            }

            // Use nullptr boundaries so the RCG solver uses the full capacity
            // as budget (reference: find_and_analyse_not_fixed_paths operates
            // on the loop subgraph with synthetic Start/End nodes that default
            // to energy_budget - chkpt_restore, not boundary block E_left).
            (void)sBound;
            (void)eBound;
            RCGSolver solver(synPath, state_, cfg_, params_, solution.blockMeta,
                             solution.decidedPlacements, nullptr, nullptr, tracker_);
            RCGResult result = solver.solve();
            if (!result.feasible) {
                PLOGE << "SCHEMATIC infeasible: loop uncovered block '" << BB->getName()
                      << "' in loop at '" << header->getName() << "': " << result.errorMessage;
                return false;
            }

            for (const auto &ckpt : result.selectedCheckpoints)
                solution.enabledCheckpoints.insert(resolveCheckpointEdge(ckpt));
            for (unsigned i = 0; i < result.intervalBlocks.size(); ++i) {
                const auto &blocks = result.intervalBlocks[i];
                const auto &alloc = result.allocations[i];
                for (llvm::BasicBlock *B : blocks) {
                    for (const auto &[gv, place] : alloc.placement)
                        solution.decidedPlacements[B][gv] = place;
                    solution.blockMeta[B].analyzed = true;
                }
                solution.regions.push_back({blocks, alloc});
            }
        }
    }

    // Step 5: Get header and latch allocations from decided placements.
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

    // Step 6: Check if allocations differ at header vs latch.
    bool allocationsDiffer = (headerAlloc != latchAlloc);
    if (allocationsDiffer) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        decision.E_loop = 0.0;
        decision.bodyAllocation = buildBoundaryAllocation(headerAlloc);
        solution.loopDecisions[header] = decision;
        propagateLoopEnergy(L, decision.bodyAllocation, solution);
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        return true;
    }

    // Step 7: Compute E_loop and nb_it_with_budget.
    // E_loop = header.E_to_leave - latch.E_to_leave + latchCost + loop_increment_cost_nvm
    //
    // The reference uses synthetic zero-cost START_Loop/END_Loop boundary nodes,
    // so first_bb.E_to_leave - last_bb.E_to_leave captures ALL block costs
    // (header through latch inclusive). We use real blocks, so header.E_to_leave
    // already includes latch's cost in the accumulator, meaning the difference
    // misses the latch's own execution cost. We add it back explicitly (latchCost).
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
    // latchCost compensates for lack of synthetic nodes (see spec Step 7).
    double latchCost = latch ? getAdjustedBlockEnergy(latch, bodyAlloc) : 0.0;
    double E_loop = headerEToLeave - latchEToLeave + latchCost + params_.loopIncrementCostNvm;
    decision.E_loop = E_loop;
    decision.bodyAllocation = bodyAlloc;

    if (E_loop <= 0.0) {
        decision.numIterationsPerCharge = 0;
        solution.loopDecisions[header] = decision;
        return true;
    }

    double availableEnergy = params_.capacity - latchEToLeave;
    if (availableEnergy <= 0.0) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        solution.loopDecisions[header] = decision;
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        return true;
    }

    int rawNumIt = static_cast<int>(std::floor(availableEnergy / E_loop)) - 1;
    auto numIt = static_cast<unsigned>(std::max(rawNumIt, 0));

    // Step 8: Convergence loop (reference lines 574-617): if loop has only disabled
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

    if (!hasEnabledCheckpoints && numIt > 1) {
        std::vector<llvm::BasicBlock *> loopBlocks = L->getBlocksVector();
        unsigned oldNumIt = 0;
        for (unsigned iter = 0; iter < 15; ++iter) {
            if (oldNumIt != 0 && numIt >= oldNumIt)
                break;
            oldNumIt = numIt;

            unsigned scaledIters =
                static_cast<unsigned>(std::min(static_cast<uint64_t>(numIt), maxTripCount));

            std::map<llvm::Value *, Placement> fixed;
            auto hdIt2 = solution.decidedPlacements.find(header);
            if (hdIt2 != solution.decidedPlacements.end())
                fixed = hdIt2->second;
            RegionAllocation newAlloc = computeIntervalAllocation(
                loopBlocks, state_, params_, fixed, tracker_, nullptr, nullptr, scaledIters);
            bodyAlloc = newAlloc;

            for (llvm::BasicBlock *BB : loopBlocks) {
                for (const auto &[gv, place] : bodyAlloc.placement)
                    solution.decidedPlacements[BB][gv] = place;
                solution.blockMeta[BB].analyzed = true;
            }

            propagateLoopEnergy(L, bodyAlloc, solution);

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

    // Step 9: Decide checkpoint type.
    if (numIt > maxTripCount) {
        // Entire loop fits — no checkpoint needed, but use maxTripCount for
        // energy scaling so propagation accounts for all iterations.
        decision.numIterationsPerCharge = static_cast<unsigned>(maxTripCount);
        decision.loopFitsEntirely = true;
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

    // Step 10: Set LoopMark on blocks and adjust energy.
    // E_left may go negative after adjustment — this is expected and correctly
    // reflects that the block's remaining energy budget is consumed by subsequent
    // iterations. Downstream propagateEnergy() handles negative E_left correctly.
    if (decision.numIterationsPerCharge > 1) {
        unsigned adjIter = decision.numIterationsPerCharge;
        if (decision.loopFitsEntirely)
            adjIter = static_cast<unsigned>(maxTripCount);
        LoopMark mark{L, adjIter, E_loop};
        double adj = (adjIter - 1) * E_loop;
        for (llvm::BasicBlock *BB : L->getBlocksVector()) {
            auto &meta = solution.blockMeta[BB];
            meta.loop = mark;
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
                // Skip back-edges: any edge from within a loop back to its header.
                if (llvm::Loop *dstLoop = LI_.getLoopFor(dstBB))
                    if (dstLoop->getHeader() == dstBB && dstLoop->contains(srcBB))
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
                // Skip back-edges: any edge from within a loop back to its header.
                if (llvm::Loop *dstLoop = LI_.getLoopFor(dstBB))
                    if (dstLoop->getHeader() == dstBB && dstLoop->contains(srcBB))
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
    // via blockMeta.loop, producing the same final values as the reference.

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
