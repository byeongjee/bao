#include "schematic/LoopAnalyzer.h"
#include "common/Logger.h"
#include "common/LoopTripCount.h"
#include "schematic/EnergyPropagation.h"
#include "schematic/MemoryAllocator.h"
#include "schematic/RCGSolver.h"
#include "schematic/TraceAnalyzer.h"

#include "llvm/ADT/SmallVector.h"

#include <cmath>
#include <limits>

namespace checkpoint {

static std::string loopOriginTag(llvm::Loop *L, llvm::StringRef reason) {
    llvm::BasicBlock *header = L ? L->getHeader() : nullptr;
    std::string headerName = header ? header->getName().str() : "<unknown>";
    return ("loop-" + reason + "[" + headerName + "]").str();
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
    llvm::BasicBlock *header = L->getHeader();
    SchematicBlock *headerBlock = graph_.getOrCreate(header);

    // Create synthetic START_Loop/END_Loop boundary blocks.
    SchematicBlock *startSynth = graph_.createSynthetic("START_Loop");
    SchematicBlock *endSynth = graph_.createSynthetic("END_Loop");

    // Cleanup: erase synthetic block entries from solution maps on all exit paths.
    // Graph owns the synthetic blocks, so no delete needed.
    auto cleanup = [&]() {
        solution.blockMeta.erase(startSynth);
        solution.blockMeta.erase(endSynth);
        solution.decidedPlacements.erase(startSynth);
        solution.decidedPlacements.erase(endSynth);
        solution.blockAllocation.erase(startSynth);
        solution.blockAllocation.erase(endSynth);
    };
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
    std::vector<std::vector<SchematicBlock *>> bodyPaths;
    for (const auto &lt : loadedLoopTraces_) {
        if (lt.header == headerBlock) {
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
        // Reference: schematic.py:544-546 — insert START_Loop and END_Loop, then analyse_trace.
        path.insert(path.begin(), startSynth);
        path.push_back(endSynth);

        // Add trace edges so SchematicBlock predecessors/successors are populated.
        graph_.addTraceEdges(path);

        std::string errorMsg;
        if (!analyzeTrace(path, solution, state_, cfg_, params_, tracker_, LI_, L, errorMsg)) {
            PLOGE << "SCHEMATIC infeasible: energy capacity too small for loop at '"
                  << header->getName() << "': " << errorMsg;
            return false;
        }
    }

    // Step 3b: Analyze uncovered blocks within this loop (reference: schematic.py:554).
    {
        std::string errorMsg;
        if (!findAndAnalyzeNotFixedPaths(cfg_, solution, state_, params_, tracker_, LI_, L, graph_,
                                         errorMsg)) {
            PLOGE << "SCHEMATIC infeasible: uncovered block in loop at '" << header->getName()
                  << "': " << errorMsg;
            return false;
        }
    }

    // Step 3c: Resolve loop-internal edges (reference: schematic.py:555).
    removePotentialCheckpointsBetweenFixedBBs(cfg_, solution, state_, params_, LI_, graph_, L);

    // Step 5: Get header and latch allocations from decided placements.
    std::map<llvm::Value *, Placement> headerAlloc;
    auto hdIt = solution.decidedPlacements.find(headerBlock);
    if (hdIt != solution.decidedPlacements.end())
        headerAlloc = hdIt->second;

    llvm::BasicBlock *latch = L->getLoopLatch();
    SchematicBlock *latchBlock = latch ? graph_.getOrCreate(latch) : nullptr;
    std::map<llvm::Value *, Placement> latchAlloc;
    if (latchBlock) {
        auto ltIt = solution.decidedPlacements.find(latchBlock);
        if (ltIt != solution.decidedPlacements.end())
            latchAlloc = ltIt->second;
    }

    LoopCheckpointDecision decision;
    decision.loop = L;

    // Step 6: Check if allocations differ at header vs latch.
    // Match Python reference: only variables present in BOTH maps with different
    // placements constitute a real conflict.  Extra NVM variables in one map but
    // not the other are implicitly compatible (NVM is the default/unplaced state).
    // VM variables absent from the other map conflict only if they would overlap
    // an existing VM allocation, but since we don't track addresses here we
    // conservatively treat any VM-in-one-but-absent-in-other as a conflict.
    bool allocationsDiffer = false;
    for (const auto &[gv, place] : headerAlloc) {
        auto it = latchAlloc.find(gv);
        if (it != latchAlloc.end()) {
            if (it->second != place) {
                allocationsDiffer = true;
                break;
            }
        } else if (place == Placement::VM) {
            allocationsDiffer = true;
            break;
        }
    }
    if (!allocationsDiffer) {
        for (const auto &[gv, place] : latchAlloc) {
            if (headerAlloc.find(gv) == headerAlloc.end() && place == Placement::VM) {
                allocationsDiffer = true;
                break;
            }
        }
    }

    // Merge: extend both maps so subsequent steps see a consistent variable set.
    // Variables absent from one side are added with the other side's placement.
    // This mirrors Python's extends_memory_allocation which merges missing vars.
    if (!allocationsDiffer) {
        for (const auto &[gv, place] : latchAlloc) {
            if (headerAlloc.find(gv) == headerAlloc.end())
                headerAlloc[gv] = place;
        }
        for (const auto &[gv, place] : headerAlloc) {
            if (latchAlloc.find(gv) == latchAlloc.end())
                latchAlloc[gv] = place;
        }
        // Update decidedPlacements with the merged maps.
        solution.decidedPlacements[headerBlock] = headerAlloc;
        if (latchBlock)
            solution.decidedPlacements[latchBlock] = latchAlloc;
    }

    if (allocationsDiffer) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        decision.E_loop = 0.0;
        decision.bodyAllocation = buildBoundaryAllocation(headerAlloc);
        solution.loopDecisions[headerBlock] = decision;

        // Propagate energy from synthetic boundaries directly.
        double energyLeftStart =
            params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
        for (const auto &[gv, va] : decision.bodyAllocation.vars) {
            if (va.placement == Placement::VM)
                energyLeftStart -= params_.memRestoreEnergyPerByte * state_.getVarSizeBytes(gv);
        }
        SchematicBlock *fwdDst =
            startSynth->successors().empty() ? nullptr : startSynth->successors()[0];
        if (fwdDst) {
            CFGEdge fwdEdge{startSynth, fwdDst};
            propagateEnergyLeft(fwdEdge, energyLeftStart, solution, cfg_, state_, params_, LI_, L);
        }
        double eToLeave =
            params_.E_epi + params_.N_reg * params_.regStoreEnergy + params_.loopIncrementCostNvm;
        for (const auto &[gv, va] : decision.bodyAllocation.vars) {
            if (va.placement == Placement::VM)
                eToLeave += params_.memStoreEnergyPerByte * state_.getVarSizeBytes(gv);
        }
        SchematicBlock *bwdSrc =
            endSynth->predecessors().empty() ? nullptr : endSynth->predecessors()[0];
        if (bwdSrc) {
            CFGEdge bwdEdge{bwdSrc, endSynth};
            propagateEnergyToLeave(bwdEdge, eToLeave, solution, cfg_, state_, params_, LI_, L);
        }
        solution.blockMeta[startSynth].E_to_leave = solution.blockMeta[headerBlock].E_to_leave;
        solution.blockMeta[startSynth].E_left = energyLeftStart;
        solution.blockMeta[endSynth].E_to_leave = eToLeave;

        if (latchBlock)
            enableCheckpoint(solution, CFGEdge{latchBlock, headerBlock},
                             loopOriginTag(L, "alloc-mismatch-backedge"));
        return true;
    }

    // Step 7: Compute E_loop and nb_it_with_budget.
    // E_loop = START_Loop.E_to_leave - END_Loop.E_to_leave + loop_increment_cost_nvm
    // (reference schematic.py:566-569, using synthetic boundary nodes).
    RegionAllocation bodyAlloc = buildBoundaryAllocation(headerAlloc);

    // Copy energy values from header/latch to synthetic boundary blocks.
    // In Python, the synthetic blocks are connected in the networkx graph, so
    // apply_memory_allocation's propagation reaches them directly. In C++, the
    // synthetic blocks now have proper edges via SchematicGraph.
    // Reference: Python's first_bb/last_bb in E_loop computation (schematic.py:566-569).
    solution.blockMeta[startSynth].E_to_leave = solution.blockMeta[headerBlock].E_to_leave;
    solution.blockMeta[startSynth].E_left = solution.blockMeta[headerBlock].E_left;
    if (latchBlock) {
        solution.blockMeta[endSynth].E_left = solution.blockMeta[latchBlock].E_left;
    }

    // Read E_loop from synthetic boundary blocks.
    // Reference: first_bb.E_to_leave - last_bb.E_to_leave + loop_increment (schematic.py:566-569).
    double startEToLeave = solution.blockMeta[startSynth].E_to_leave;
    double endEToLeave = solution.blockMeta[endSynth].E_to_leave;
    double E_loop = startEToLeave - endEToLeave + params_.loopIncrementCostNvm;

    // Inner loop multi-iteration costs are handled by Step 10's direct
    // E_to_leave/E_left adjustment on loop blocks (matching Python reference
    // schematic.py:643-664) and by propagation's seenLoops scaling (which
    // applies unconditionally, without loopScope filtering).

    decision.E_loop = E_loop;
    decision.bodyAllocation = bodyAlloc;

    PLOGI << "[LoopAnalyzer] loop=" << header->getName() << " E_loop=" << E_loop
          << " startEToLeave=" << startEToLeave << " endEToLeave=" << endEToLeave
          << " capacity=" << params_.capacity << " maxTripCount=" << maxTripCount;

    if (E_loop <= 0.0) {
        decision.numIterationsPerCharge = 0;
        solution.loopDecisions[headerBlock] = decision;
        return true;
    }

    double availableEnergy = params_.capacity - endEToLeave;
    if (availableEnergy <= 0.0) {
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        solution.loopDecisions[headerBlock] = decision;
        if (latchBlock)
            enableCheckpoint(solution, CFGEdge{latchBlock, headerBlock},
                             loopOriginTag(L, "nonpositive-available-energy"));
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
        if (ckpt.src->getLLVMBlock() && L->contains(ckpt.src->getLLVMBlock()) &&
            ckpt.dst->getLLVMBlock() && L->contains(ckpt.dst->getLLVMBlock())) {
            hasEnabledCheckpoints = true;
            break;
        }
    }

    if (!hasEnabledCheckpoints && numIt > 1) {
        // Collect memory allocations from loop blocks now (after Steps 3-3c).
        // Must be collected here (not before Step 3) because RCG analysis and
        // edge resolution can modify blockAllocation, invalidating earlier pointers.
        // Reference: schematic.py:533-537.
        std::vector<const RegionAllocation *> loopMemoryAllocations;
        for (llvm::BasicBlock *BB : L->getBlocksVector()) {
            SchematicBlock *block = graph_.getOrCreate(BB);
            auto it = solution.blockAllocation.find(block);
            if (it != solution.blockAllocation.end()) {
                const RegionAllocation *ptr = it->second.get();
                if (std::find(loopMemoryAllocations.begin(), loopMemoryAllocations.end(), ptr) ==
                    loopMemoryAllocations.end())
                    loopMemoryAllocations.push_back(ptr);
            }
        }

        std::vector<SchematicBlock *> loopBlocks;
        for (llvm::BasicBlock *BB : L->getBlocksVector())
            loopBlocks.push_back(graph_.getOrCreate(BB));

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
            // Reference: schematic.py:594-596 — set_memory_allocation extends
            // existing allocation (adds missing variables, keeps existing placements).
            auto sharedAlloc = std::make_shared<RegionAllocation>(bodyAlloc);
            for (SchematicBlock *block : loopBlocks) {
                solution.blockMeta[block].analyzed = false;
                solution.blockMeta[block].E_left = std::numeric_limits<double>::max();
                solution.blockMeta[block].E_to_leave = 0.0;
                auto &placements = solution.decidedPlacements[block];
                for (const auto &[gv, va] : bodyAlloc.vars) {
                    if (placements.find(gv) == placements.end())
                        placements[gv] = va.placement;
                }
                solution.blockMeta[block].analyzed = true;
                auto existingIt = solution.blockAllocation.find(block);
                if (existingIt != solution.blockAllocation.end()) {
                    extendsAllocation(*existingIt->second, *sharedAlloc);
                } else {
                    solution.blockAllocation[block] = sharedAlloc;
                }
            }

            // Direct energy propagation from synthetic boundaries.
            // Forward seed from startSynth
            double energyLeftStart =
                params_.capacity - params_.E_pro - params_.N_reg * params_.regRestoreEnergy;
            for (const auto &[gv, va] : bodyAlloc.vars) {
                if (va.placement == Placement::VM)
                    energyLeftStart -= params_.memRestoreEnergyPerByte * state_.getVarSizeBytes(gv);
            }
            SchematicBlock *fwdDst =
                startSynth->successors().empty() ? nullptr : startSynth->successors()[0];
            if (fwdDst) {
                CFGEdge fwdEdge{startSynth, fwdDst};
                propagateEnergyLeft(fwdEdge, energyLeftStart, solution, cfg_, state_, params_, LI_,
                                    L);
            }

            // Backward seed from endSynth
            double eToLeave = params_.E_epi + params_.N_reg * params_.regStoreEnergy +
                              params_.loopIncrementCostNvm;
            for (const auto &[gv, va] : bodyAlloc.vars) {
                if (va.placement == Placement::VM)
                    eToLeave += params_.memStoreEnergyPerByte * state_.getVarSizeBytes(gv);
            }
            SchematicBlock *bwdSrc =
                endSynth->predecessors().empty() ? nullptr : endSynth->predecessors()[0];
            if (bwdSrc) {
                CFGEdge bwdEdge{bwdSrc, endSynth};
                propagateEnergyToLeave(bwdEdge, eToLeave, solution, cfg_, state_, params_, LI_, L);
            }

            // Copy to synthetic blocks
            solution.blockMeta[startSynth].E_to_leave = solution.blockMeta[headerBlock].E_to_leave;
            solution.blockMeta[startSynth].E_left = energyLeftStart;
            solution.blockMeta[endSynth].E_to_leave = eToLeave;

            // Update decidedPlacements for synthetic blocks to match current allocation
            auto hdPlIt = solution.decidedPlacements.find(headerBlock);
            if (hdPlIt != solution.decidedPlacements.end()) {
                solution.decidedPlacements[startSynth] = hdPlIt->second;
                solution.decidedPlacements[endSynth] = hdPlIt->second;
            }

            // Re-read from synthetic blocks after re-propagation.
            startEToLeave = solution.blockMeta[startSynth].E_to_leave;
            endEToLeave = solution.blockMeta[endSynth].E_to_leave;
            E_loop = startEToLeave - endEToLeave + params_.loopIncrementCostNvm;
            decision.E_loop = E_loop;
            decision.bodyAllocation = bodyAlloc;

            if (E_loop <= 0.0)
                break;

            // Convergence uses startEToLeave (not endEToLeave), matching reference
            // schematic.py:614. Propagation applies seenLoops for all loops
            // unconditionally, so startEToLeave already includes inner loop scaling.
            availableEnergy = params_.capacity - startEToLeave;
            if (availableEnergy <= 0.0)
                break;
            rawNumIt = static_cast<int>(std::floor(availableEnergy / E_loop)) - 1;
            numIt = static_cast<unsigned>(std::max(rawNumIt, 0));
        }
    }

    // Step 9: Decide checkpoint type.
    PLOGI << "[LoopAnalyzer] loop=" << header->getName() << " numIt=" << numIt
          << " maxTripCount=" << maxTripCount << " availableEnergy=" << availableEnergy;
    if (numIt > maxTripCount) {
        // Entire loop fits — no checkpoint needed, but use maxTripCount for
        // energy scaling so propagation accounts for all iterations.
        decision.numIterationsPerCharge = static_cast<unsigned>(maxTripCount);
        decision.loopFitsEntirely = true;
    } else if (numIt < 3) {
        // Too few iterations per charge — checkpoint every iteration.
        decision.mandatoryBackEdge = true;
        decision.numIterationsPerCharge = 1;
        if (latchBlock)
            enableCheckpoint(solution, CFGEdge{latchBlock, headerBlock},
                             loopOriginTag(L, "mandatory-backedge"));
    } else {
        // Conditional checkpoint every numIt iterations.
        decision.numIterationsPerCharge = numIt;
    }

    solution.loopDecisions[headerBlock] = decision;

    // Step 10: Set LoopMark on blocks and adjust energy (reference: schematic.py:643-664).
    // Directly adjust E_to_leave/E_left on all loop blocks to account for
    // multi-iteration cost. E_left may go negative — this is expected and
    // correctly reflects that the block's remaining energy budget is consumed
    // by subsequent iterations.
    if (decision.numIterationsPerCharge > 1) {
        unsigned adjIter = decision.numIterationsPerCharge;
        if (decision.loopFitsEntirely)
            adjIter = static_cast<unsigned>(maxTripCount);
        LoopMark mark{L, adjIter, E_loop};
        double adjustment = (adjIter - 1) * E_loop;
        for (llvm::BasicBlock *BB : L->getBlocksVector()) {
            SchematicBlock *block = graph_.getOrCreate(BB);
            auto &meta = solution.blockMeta[block];
            meta.loop = mark;
            meta.E_to_leave += adjustment;
            meta.E_left -= adjustment;
        }
    }

    return true;
}

bool LoopAnalyzer::analyzeLoops(SchematicSolution &solution) {
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
