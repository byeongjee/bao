#include "schematic/LoopAnalyzer.h"
#include "common/Logger.h"
#include "common/LoopTripCount.h"
#include "schematic/EnergyPropagation.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/RCGSolver.h"
#include "schematic/TraceAnalyzer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cmath>
#include <limits>

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
    llvm::BasicBlock *header = L->getHeader();

    // Create synthetic START_Loop/END_Loop boundary blocks (detached from function).
    // These match the reference Python's zero-cost synthetic nodes used as RCG
    // boundaries and energy anchor points.
    llvm::BasicBlock *startSynth = llvm::BasicBlock::Create(header->getContext(), "START_Loop");
    llvm::BasicBlock *endSynth = llvm::BasicBlock::Create(header->getContext(), "END_Loop");

    // RAII cleanup: remove synthetic block entries and delete blocks on all exit paths.
    auto cleanup = [&]() {
        solution.blockMeta.erase(startSynth);
        solution.blockMeta.erase(endSynth);
        solution.decidedPlacements.erase(startSynth);
        solution.decidedPlacements.erase(endSynth);
        solution.blockAllocation.erase(startSynth);
        solution.blockAllocation.erase(endSynth);
        startSynth->deleteValue();
        endSynth->deleteValue();
    };
    // Wrap all return paths: on scope exit, run cleanup.
    struct ScopeGuard {
        std::function<void()> fn;
        ~ScopeGuard() { fn(); }
    } guard{cleanup};

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

    // Collect existing memory allocations from loop blocks before analysis.
    // Reference: schematic.py:533-537 — collected before loop trace analysis.
    std::vector<const RegionAllocation *> loopMemoryAllocations;
    for (llvm::BasicBlock *BB : L->getBlocksVector()) {
        auto it = solution.blockAllocation.find(BB);
        if (it != solution.blockAllocation.end()) {
            const RegionAllocation *ptr = it->second.get();
            if (std::find(loopMemoryAllocations.begin(), loopMemoryAllocations.end(), ptr) ==
                loopMemoryAllocations.end())
                loopMemoryAllocations.push_back(ptr);
        }
    }

    // Step 3: Run RCG solver on each body path.
    // Initialize synthetic boundary blocks with default energy values.
    // START_Loop: default restore budget (no VM costs since allocation is undecided).
    solution.blockMeta[startSynth].E_left =
        params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
    solution.blockMeta[startSynth].analyzed = true;
    solution.decidedPlacements[startSynth] = {};

    // END_Loop: basic save cost only (no VM costs, no loopIncrementCostNvm).
    solution.blockMeta[endSynth].E_to_leave =
        params_.E_epi + params_.N_reg * params_.regStoreEnergy;
    solution.blockMeta[endSynth].analyzed = true;
    solution.decidedPlacements[endSynth] = {};

    for (auto path : bodyPaths) {
        // Reference: schematic.py:544-545 — insert START_Loop and END_Loop into trace.
        path.insert(path.begin(), startSynth);
        path.push_back(endSynth);
        RCGSolver solver(path, state_, cfg_, params_, solution.blockMeta, solution.blockAllocation,
                         tracker_);
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
            auto sharedAlloc = std::make_shared<RegionAllocation>(alloc);
            for (llvm::BasicBlock *BB : blocks) {
                for (const auto &[gv, va] : alloc.vars)
                    solution.decidedPlacements[BB][gv] = va.placement;
                solution.blockMeta[BB].analyzed = true;
                solution.blockAllocation[BB] = sharedAlloc;
            }
            solution.regions.push_back({blocks, alloc});
        }

        // Per-checkpoint energy propagation after RCG solve (reference: apply_memory_allocation
        // lines 449-466)
        {
            struct SeedCkpt {
                llvm::BasicBlock *bbBefore;
                llvm::BasicBlock *bbAfter;
                bool isVirtual;
            };
            std::vector<SeedCkpt> ckpts;
            ckpts.push_back({startSynth, path.front(), /*isVirtual=*/true});
            for (const auto &ckptEdge : result.selectedCheckpoints)
                ckpts.push_back({ckptEdge.src, ckptEdge.dst, /*isVirtual=*/false});
            ckpts.push_back({path.back(), endSynth, /*isVirtual=*/true});

            for (const auto &ck : ckpts) {
                if (ck.bbAfter) {
                    double energyLeftStart;
                    auto metaIt = solution.blockMeta.find(ck.bbAfter);
                    if (ck.isVirtual && metaIt != solution.blockMeta.end() &&
                        metaIt->second.E_left < std::numeric_limits<double>::max()) {
                        energyLeftStart =
                            metaIt->second.E_left + cfg_.getBlockInfo(ck.bbAfter).energyCost;
                        // Adjust for VM savings (same as getBlockExecEnergy but inline)
                        auto allocIt = solution.decidedPlacements.find(ck.bbAfter);
                        if (allocIt != solution.decidedPlacements.end()) {
                            for (const auto &[gv, place] : allocIt->second) {
                                if (place == Placement::VM)
                                    energyLeftStart += params_.nvmAccessPenalty *
                                                       (state_.getLoadCount(ck.bbAfter, gv) +
                                                        state_.getStoreCount(ck.bbAfter, gv));
                            }
                        }
                    } else {
                        energyLeftStart = params_.capacity - params_.E_pro -
                                          params_.N_reg * params_.regRestoreEnergy;
                        auto allocIt = solution.decidedPlacements.find(ck.bbAfter);
                        if (allocIt != solution.decidedPlacements.end()) {
                            for (const auto &[gv, place] : allocIt->second) {
                                if (place == Placement::VM)
                                    energyLeftStart -= params_.memRestoreEnergyPerByte *
                                                       state_.getVarSizeBytes(gv);
                            }
                        }
                    }
                    CFGEdge fwdEdge{ck.bbBefore, ck.bbAfter};
                    propagateEnergyLeft(fwdEdge, energyLeftStart, solution, cfg_, state_, params_,
                                        LI_, /*loopScope=*/L);
                }

                if (ck.bbBefore) {
                    double eToLeave;
                    auto metaIt = solution.blockMeta.find(ck.bbBefore);
                    if (metaIt != solution.blockMeta.end() && metaIt->second.E_to_leave != 0.0) {
                        eToLeave =
                            metaIt->second.E_to_leave - cfg_.getBlockInfo(ck.bbBefore).energyCost;
                        auto allocIt = solution.decidedPlacements.find(ck.bbBefore);
                        if (allocIt != solution.decidedPlacements.end()) {
                            for (const auto &[gv, place] : allocIt->second) {
                                if (place == Placement::VM)
                                    eToLeave += params_.nvmAccessPenalty *
                                                (state_.getLoadCount(ck.bbBefore, gv) +
                                                 state_.getStoreCount(ck.bbBefore, gv));
                            }
                        }
                    } else {
                        eToLeave = params_.E_epi + params_.N_reg * params_.regStoreEnergy;
                        auto allocIt = solution.decidedPlacements.find(ck.bbBefore);
                        if (allocIt != solution.decidedPlacements.end()) {
                            for (const auto &[gv, place] : allocIt->second) {
                                if (place == Placement::VM)
                                    eToLeave +=
                                        params_.memStoreEnergyPerByte * state_.getVarSizeBytes(gv);
                            }
                        }
                    }
                    CFGEdge bwdEdge{ck.bbBefore, ck.bbAfter};
                    propagateEnergyToLeave(bwdEdge, eToLeave, solution, cfg_, state_, params_, LI_,
                                           /*loopScope=*/L);
                }
            }

            // Update synthetic boundary block metadata after propagation
            solution.blockMeta[startSynth].E_to_leave = solution.blockMeta[header].E_to_leave;
            solution.blockMeta[startSynth].E_left =
                params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
            for (const auto &[gv, place] : solution.decidedPlacements[startSynth]) {
                if (place == Placement::VM)
                    solution.blockMeta[startSynth].E_left -=
                        params_.memRestoreEnergyPerByte * state_.getVarSizeBytes(gv);
            }
            solution.blockMeta[endSynth].E_to_leave = params_.E_epi +
                                                      params_.N_reg * params_.regStoreEnergy +
                                                      params_.loopIncrementCostNvm;
            for (const auto &[gv, place] : solution.decidedPlacements[endSynth]) {
                if (place == Placement::VM)
                    solution.blockMeta[endSynth].E_to_leave +=
                        params_.memStoreEnergyPerByte * state_.getVarSizeBytes(gv);
            }
        }
    }

    // Step 3b: Analyze uncovered blocks within this loop (reference: schematic.py:554).
    {
        std::string errorMsg;
        if (!findAndAnalyzeNotFixedPaths(cfg_, solution, state_, params_, tracker_, LI_, L,
                                         errorMsg)) {
            PLOGE << "SCHEMATIC infeasible: uncovered block in loop at '" << header->getName()
                  << "': " << errorMsg;
            return false;
        }
    }

    // Step 3c: Resolve loop-internal edges (reference: schematic.py:555).
    removePotentialCheckpointsBetweenFixedBBs(cfg_, solution, state_, params_, LI_, L);

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
        propagateFromBoundaries(L, decision.bodyAllocation, solution, startSynth, endSynth);
        if (latch)
            solution.enabledCheckpoints.insert(CFGEdge{latch, header});
        return true;
    }

    // Step 7: Compute E_loop and nb_it_with_budget.
    // E_loop = START_Loop.E_to_leave - END_Loop.E_to_leave + loop_increment_cost_nvm
    // (reference schematic.py:566-569, using synthetic boundary nodes).
    RegionAllocation bodyAlloc = buildBoundaryAllocation(headerAlloc);

    // Run initial energy propagation before computing E_loop — without this,
    // E_to_leave/E_left are at defaults (0.0 / max) since RCG does not set them.
    propagateFromBoundaries(L, bodyAlloc, solution, startSynth, endSynth);

    // Read E_loop from synthetic boundary blocks.
    // Reference: first_bb.E_to_leave - last_bb.E_to_leave + loop_increment (schematic.py:566-569).
    double startEToLeave = solution.blockMeta[startSynth].E_to_leave;
    double endEToLeave = solution.blockMeta[endSynth].E_to_leave;
    double E_loop = startEToLeave - endEToLeave + params_.loopIncrementCostNvm;

    decision.E_loop = E_loop;
    decision.bodyAllocation = bodyAlloc;

    if (E_loop <= 0.0) {
        decision.numIterationsPerCharge = 0;
        solution.loopDecisions[header] = decision;
        return true;
    }

    double availableEnergy = params_.capacity - endEToLeave;
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

            // Reference: schematic.py:589-590 — pass None, None for start/end alloc,
            // and memory_allocations collected before analysis.
            auto [newAlloc, _gain] =
                chooseMemoryAllocation(loopBlocks, state_, params_, nullptr, nullptr,
                                       loopMemoryAllocations, tracker_, scaledIters);
            bodyAlloc = newAlloc;

            // Reference: schematic.py:594-596 — reset then set in single loop:
            //   bb.is_fixed = False
            //   bb.set_memory_allocation(mem_alloc, allocator)
            // set_memory_allocation sets is_fixed=True and recomputes final_cost.
            auto sharedAlloc = std::make_shared<RegionAllocation>(bodyAlloc);
            for (llvm::BasicBlock *BB : loopBlocks) {
                solution.blockMeta[BB].analyzed = false;
                solution.blockMeta[BB].E_left = std::numeric_limits<double>::max();
                solution.blockMeta[BB].E_to_leave = 0.0;
                for (const auto &[gv, va] : bodyAlloc.vars)
                    solution.decidedPlacements[BB][gv] = va.placement;
                solution.blockMeta[BB].analyzed = true;
                solution.blockAllocation[BB] = sharedAlloc;
            }

            propagateFromBoundaries(L, bodyAlloc, solution, startSynth, endSynth);

            // Re-read from synthetic blocks after re-propagation.
            startEToLeave = solution.blockMeta[startSynth].E_to_leave;
            endEToLeave = solution.blockMeta[endSynth].E_to_leave;
            E_loop = startEToLeave - endEToLeave + params_.loopIncrementCostNvm;
            decision.E_loop = E_loop;
            decision.bodyAllocation = bodyAlloc;

            if (E_loop <= 0.0)
                break;

            // Convergence uses startEToLeave (not endEToLeave), matching reference
            // schematic.py:614. After re-propagation, START_Loop's accumulated energy
            // reflects the true worst-case cost to traverse the entire loop body.
            availableEnergy = params_.capacity - startEToLeave;
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

void LoopAnalyzer::propagateFromBoundaries(llvm::Loop *L, const RegionAllocation &alloc,
                                           SchematicSolution &solution,
                                           llvm::BasicBlock *startSynth,
                                           llvm::BasicBlock *endSynth) {
    llvm::BasicBlock *header = L->getHeader();
    llvm::BasicBlock *latch = L->getLoopLatch();

    // Forward from startSynth (reference lines 599-603)
    double energyLeftStart =
        params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
    for (const auto &[gv, va] : alloc.vars) {
        if (va.placement == Placement::VM)
            energyLeftStart -= params_.memRestoreEnergyPerByte * state_.getVarSizeBytes(gv);
    }
    CFGEdge fwdEdge{startSynth, header};
    propagateEnergyLeft(fwdEdge, energyLeftStart, solution, cfg_, state_, params_, LI_, L);

    // Backward from endSynth (reference lines 604-606)
    double eToLeave =
        params_.E_epi + params_.N_reg * params_.regStoreEnergy + params_.loopIncrementCostNvm;
    for (const auto &[gv, va] : alloc.vars) {
        if (va.placement == Placement::VM)
            eToLeave += params_.memStoreEnergyPerByte * state_.getVarSizeBytes(gv);
    }
    if (latch) {
        CFGEdge bwdEdge{latch, endSynth};
        propagateEnergyToLeave(bwdEdge, eToLeave, solution, cfg_, state_, params_, LI_, L);
    }

    // Update synthetic boundary block metadata
    solution.blockMeta[startSynth].E_to_leave = solution.blockMeta[header].E_to_leave;
    solution.blockMeta[startSynth].E_left = energyLeftStart;
    solution.blockMeta[endSynth].E_to_leave = eToLeave;

    // Update decidedPlacements for synthetic blocks to match current allocation
    auto hdPlIt = solution.decidedPlacements.find(header);
    if (hdPlIt != solution.decidedPlacements.end()) {
        solution.decidedPlacements[startSynth] = hdPlIt->second;
        solution.decidedPlacements[endSynth] = hdPlIt->second;
    }
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
