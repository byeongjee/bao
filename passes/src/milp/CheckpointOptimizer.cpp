#include "milp/CheckpointOptimizer.h"

#include "common/Logger.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <vector>

#define DEBUG_TYPE "checkpoint-optimizer"

namespace checkpoint {

namespace {

static std::string sanitizeToken(llvm::StringRef input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

static std::string nodeToken(const ICFGView &cfg, NodeId block) {
    return sanitizeToken(cfg.getNodeName(block));
}

/// Return a sanitized token for any Value (named or unnamed).
static std::string valueToken(llvm::Value *V) {
    if (V->hasName())
        return sanitizeToken(V->getName());
    std::string raw;
    llvm::raw_string_ostream rso(raw);
    V->printAsOperand(rso, false);
    return sanitizeToken(rso.str());
}

static std::string makeVarName(const ICFGView &cfg, const char *prefix, NodeId block,
                               llvm::Value *v) {
    return std::string(prefix) + "_" + nodeToken(cfg, block) + "_" + valueToken(v);
}

static std::string makeGlobalVarName(const char *prefix, llvm::Value *v) {
    return std::string(prefix) + "_" + valueToken(v);
}

template <typename T> static std::vector<T *> collectSortedValues(const std::vector<T *> &values) {
    std::vector<T *> ordered(values.begin(), values.end());
    stableSortAndUniqueValues(ordered);
    return ordered;
}

template <typename T> static std::vector<T *> collectSortedValues(const std::set<T *> &values) {
    std::vector<T *> ordered(values.begin(), values.end());
    stableSortAndUniqueValues(ordered);
    return ordered;
}

static std::vector<llvm::Value *> collectSortedLiveInValues(const IStateView &state, NodeId block) {
    std::vector<llvm::Value *> ordered;
    for (llvm::GlobalVariable *GV : state.getEligLiveIn(block))
        ordered.push_back(GV);
    for (llvm::Value *V : state.getIneligLiveIn(block))
        ordered.push_back(V);
    stableSortAndUniqueValues(ordered);
    return ordered;
}

static std::map<NodeId, std::vector<NodeId>> buildPredecessorMap(const ICFGView &cfg) {
    std::map<NodeId, std::vector<NodeId>> preds;
    for (NodeId block : cfg.getBlocks()) {
        preds[block] = {};
    }
    for (const auto &[src, dst] : cfg.getEdges()) {
        preds[dst].push_back(src);
    }
    return preds;
}

static std::map<NodeId, std::vector<NodeId>> buildSuccessorMap(const ICFGView &cfg) {
    std::map<NodeId, std::vector<NodeId>> succs;
    for (NodeId block : cfg.getBlocks()) {
        succs[block] = {};
    }
    for (const auto &[src, dst] : cfg.getEdges()) {
        succs[src].push_back(dst);
    }
    return succs;
}

/// Compute Reach(v) for each variable: forward-reachable closure from definition sites.
static std::map<llvm::Value *, std::set<NodeId>>
computeReachableDefs(const ICFGView &cfg, const IStateView &state,
                     const std::vector<llvm::Value *> &allTracked,
                     const std::map<NodeId, std::vector<NodeId>> &succs) {
    std::map<llvm::Value *, std::set<NodeId>> reach;
    for (llvm::Value *V : allTracked) {
        // BFS from definition sites.
        std::set<NodeId> &reachSet = reach[V];
        std::vector<NodeId> worklist;
        for (NodeId block : cfg.getBlocks()) {
            bool isDef = false;
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
                isDef = state.getEligDefIndicator(block, GV);
            if (!isDef)
                isDef = state.getIneligDefIndicator(block, V);
            if (isDef) {
                reachSet.insert(block);
                worklist.push_back(block);
            }
        }
        while (!worklist.empty()) {
            NodeId cur = worklist.back();
            worklist.pop_back();
            auto it = succs.find(cur);
            if (it == succs.end())
                continue;
            for (NodeId succ : it->second) {
                if (reachSet.insert(succ).second) {
                    worklist.push_back(succ);
                }
            }
        }
    }
    return reach;
}

} // namespace

CheckpointOptimizer::CheckpointOptimizer(const MILPInput &input)
    : cfg_(input.cfg), state_(input.state), energy_(input.energy),
      params_(input.energy.getParams()) {}

const MILPProblemSizeStats &CheckpointOptimizer::getProblemSizeStats() {
    computeProblemSizeStats();
    return problemSizeStats_;
}

int CheckpointOptimizer::getNumVars() {
    return getProblemSizeStats().variablesBeforePresolve;
}

int CheckpointOptimizer::getNumConstrs() {
    return getProblemSizeStats().constraintsBeforePresolve;
}

int CheckpointOptimizer::getNumPresolvedVars() {
    return getProblemSizeStats().variablesAfterPresolve;
}

int CheckpointOptimizer::getNumPresolvedConstrs() {
    return getProblemSizeStats().constraintsAfterPresolve;
}

std::vector<NodeId> CheckpointOptimizer::getInfeasibleBlocks() const {
    std::vector<NodeId> infeasible;
    for (NodeId block : cfg_.getBlocks()) {
        if (cfg_.getBlockEnergyCost(block) > params_.capacity) {
            infeasible.push_back(block);
        }
    }
    return infeasible;
}

bool CheckpointOptimizer::solve() {
    auto infeasible = getInfeasibleBlocks();
    if (!infeasible.empty()) {
        PLOGE << "Error: Blocks exceed capacity";
        return false;
    }

    if (!ensureModelBuilt())
        return false;
    computeProblemSizeStats();

    try {
        if (timeLimit_ > 0.0)
            model_->set(GRB_DoubleParam_TimeLimit, timeLimit_);
        if (mipGap_ > 0.0)
            model_->set(GRB_DoubleParam_MIPGap, mipGap_);
        if (!logFile_.empty()) {
            model_->set(GRB_IntParam_OutputFlag, 1);
            model_->set(GRB_StringParam_LogFile, logFile_);
        }
        model_->optimize();
        solved_ = true;

        int status = model_->get(GRB_IntAttr_Status);
        if (status == GRB_OPTIMAL) {
            extractSolution();
            solution_.solverStatus = SolverStatus::Optimal;
            return true;
        }

        int solCount = model_->get(GRB_IntAttr_SolCount);
        PLOGE << "Optimization did not prove optimality"
              << " (Gurobi status=" << status << ", solutions found=" << solCount << ")";

        if (status == GRB_INFEASIBLE) {
            PLOGE << "Optimization infeasible; computing IIS diagnostics...";
            model_->computeIIS();
            model_->write("milp_infeasible.lp");
            model_->write("milp_infeasible.ilp");
            PLOGD << "  Wrote model: milp_infeasible.lp";
            PLOGD << "  Wrote IIS:   milp_infeasible.ilp";

            // getConstrs()/getVars() return caller-owned arrays.
            std::unique_ptr<GRBConstr[]> constrs(model_->getConstrs());
            int numConstrs = model_->get(GRB_IntAttr_NumConstrs);
            int printedConstrs = 0;
            for (int i = 0; i < numConstrs; i++) {
                if (!constrs[i].get(GRB_IntAttr_IISConstr))
                    continue;
                PLOGD << "  IIS constr: " << constrs[i].get(GRB_StringAttr_ConstrName);
                printedConstrs++;
                if (printedConstrs >= 200) {
                    PLOGD << "  IIS constr: ... truncated at 200 entries";
                    break;
                }
            }

            std::unique_ptr<GRBVar[]> vars(model_->getVars());
            int numVars = model_->get(GRB_IntAttr_NumVars);
            int printedVarBounds = 0;
            for (int i = 0; i < numVars; i++) {
                if (!vars[i].get(GRB_IntAttr_IISLB) && !vars[i].get(GRB_IntAttr_IISUB))
                    continue;
                PLOGD << "  IIS var bound: " << vars[i].get(GRB_StringAttr_VarName)
                      << " LB=" << vars[i].get(GRB_IntAttr_IISLB)
                      << " UB=" << vars[i].get(GRB_IntAttr_IISUB);
                printedVarBounds++;
                if (printedVarBounds >= 200) {
                    PLOGD << "  IIS var bound: ... truncated at 200 entries";
                    break;
                }
            }
        }

        if (acceptFeasible_ && solCount > 0) {
            double gap = model_->get(GRB_DoubleAttr_MIPGap);
            PLOGW << "Accepting feasible solution (MIP gap=" << gap << ")";
            extractSolution();
            solution_.solverStatus = SolverStatus::Feasible;
            solution_.mipGap = gap;
            return true;
        }

        return false;
    } catch (const GRBException &e) {
        PLOGE << "Gurobi error during MILP solve: " << e.getMessage() << " (code "
              << e.getErrorCode() << ")";
    } catch (const std::exception &e) {
        PLOGE << "Error during MILP solve: " << e.what();
    }
    return false;
}

bool CheckpointOptimizer::ensureModelBuilt() {
    if (modelBuilt_)
        return true;
    if (gurobiFailed_)
        return false;

    // This translation unit compiles with -fexceptions (unlike the rest of
    // the plugin) so Gurobi errors — license failures in the GRBEnv
    // constructor, out-of-memory during model construction — surface as
    // error returns instead of std::terminate.
    try {
        env_ = std::make_unique<GRBEnv>();
        model_ = std::make_unique<GRBModel>(*env_);
        model_->set(GRB_IntParam_OutputFlag, 0);
        model_->set(GRB_IntParam_Threads, 1);
        model_->set(GRB_IntParam_Seed, 0);
        model_->set(GRB_IntParam_ConcurrentMIP, 1);
        model_->set(GRB_IntParam_MIPFocus, 1);
        model_->set(GRB_IntParam_Presolve, 2);
        model_->set(GRB_IntParam_Cuts, 2);
        model_->set(GRB_IntParam_Symmetry, 2);
        model_->set(GRB_DoubleParam_Heuristics, 0.1);
        buildModel();
        modelBuilt_ = true;
        return true;
    } catch (const GRBException &e) {
        PLOGE << "Gurobi error while building MILP model: " << e.getMessage() << " (code "
              << e.getErrorCode() << ")";
    } catch (const std::exception &e) {
        PLOGE << "Error while building MILP model: " << e.what();
    }
    gurobiFailed_ = true;
    env_.reset();
    model_.reset();
    return false;
}

void CheckpointOptimizer::computeProblemSizeStats() {
    if (!ensureModelBuilt())
        return;

    try {
        if (!baseSizeStatsComputed_) {
            model_->update();
            problemSizeStats_.variablesBeforePresolve = model_->get(GRB_IntAttr_NumVars);
            problemSizeStats_.constraintsBeforePresolve = model_->get(GRB_IntAttr_NumConstrs);
            baseSizeStatsComputed_ = true;
        }

        // Presolve must run BEFORE optimize(): afterwards Gurobi uses the
        // incumbent objective as a cutoff and presolve reduces the model to
        // nothing, reporting 0 variables/constraints. GRBModel::presolve()
        // throws GRBException on an infeasible model; the catch below leaves
        // the after-presolve fields at 0 in that case (marked computed so it
        // is not retried).
        if (!presolveStatsComputed_ && !solved_) {
            presolveStatsComputed_ = true;
            GRBModel presolved = model_->presolve();
            problemSizeStats_.variablesAfterPresolve = presolved.get(GRB_IntAttr_NumVars);
            problemSizeStats_.constraintsAfterPresolve = presolved.get(GRB_IntAttr_NumConstrs);
        }
    } catch (const GRBException &e) {
        PLOGE << "Gurobi error while computing problem-size stats: " << e.getMessage() << " (code "
              << e.getErrorCode() << ")";
    }
}

void CheckpointOptimizer::buildModel() {
    predecessors_ = buildPredecessorMap(cfg_);
    auto succs = buildSuccessorMap(cfg_);
    orderedVmObjs_ = collectSortedValues(state_.getVMObjs());

    // Build combined list of all tracked variables.
    orderedTrackedValues_.clear();
    for (llvm::GlobalVariable *GV : orderedVmObjs_)
        orderedTrackedValues_.push_back(static_cast<llvm::Value *>(GV));
    orderedTrackedValues_.insert(orderedTrackedValues_.end(), state_.getIneligibleObjs().begin(),
                                 state_.getIneligibleObjs().end());
    stableSortAndUniqueValues(orderedTrackedValues_);

    // Precompute Reach(v) for scoping d[b,v] variables.
    reachableDefs_ = computeReachableDefs(cfg_, state_, orderedTrackedValues_, succs);
    rHatKeys_.clear();
    sKeys_.clear();

    addVariables();
    addObjective();
    addConstraints();
    model_->update();
}

void CheckpointOptimizer::addVariables() {
    const double Ebuf = params_.capacity;
    NodeId entry = cfg_.getEntryBlock();

    // r_b for all blocks.
    for (NodeId block : cfg_.getBlocks()) {
        r_[block] = model_->addVar(0.0, 1.0, 0.0, GRB_BINARY, "r_" + nodeToken(cfg_, block));
    }

    if (coarseAllocation_) {
        for (llvm::GlobalVariable *GV : orderedVmObjs_) {
            mGlobal_[GV] = model_->addVar(0.0, 1.0, 0.0, GRB_BINARY, makeGlobalVarName("m", GV));
        }
    }

    // m_{b,v} for eligible variables only in fine mode
    // (ineligibles have m=1 constant).
    // d_{b,v} only for (block, var) where b ∈ Reach(v).
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : orderedTrackedValues_) {
            BlockVarKey key = std::make_pair(block, V);
            if (!coarseAllocation_ && !state_.isIneligible(V)) {
                m_[key] =
                    model_->addVar(0.0, 1.0, 0.0, GRB_BINARY, makeVarName(cfg_, "m", block, V));
            }
            if (reachableDefs_[V].count(block)) {
                d_[key] =
                    model_->addVar(0.0, 1.0, 0.0, GRB_CONTINUOUS, makeVarName(cfg_, "d", block, V));
            }
        }
    }

    // rHat_{b,v} only for eligible + live-in pairs.
    // Ineligible+live-in: rHat = r[b], substituted directly in buildEStart.
    // Not live-in: rHat = 0, no variable needed.
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : collectSortedValues(state_.getEligLiveIn(block))) {
            BlockVarKey key = std::make_pair(block, static_cast<llvm::Value *>(GV));
            rHat_[key] =
                model_->addVar(0.0, 1.0, 0.0, GRB_CONTINUOUS, makeVarName(cfg_, "rHat", block, GV));
            rHatKeys_.push_back(key);
        }
    }

    // s_{b,v} for b != entry, v in LiveIn(b).
    // Skip when no predecessor has d[pred,v] (s <= Σ_pred d = 0).
    for (NodeId block : cfg_.getBlocks()) {
        if (block == entry)
            continue;
        for (llvm::Value *V : collectSortedLiveInValues(state_, block)) {
            // Skip if no predecessor is in Reach(v) (d[pred,v] = 0 for all preds).
            bool hasDirtyPred = false;
            for (NodeId pred : predecessors_[block]) {
                if (d_.count(std::make_pair(pred, V))) {
                    hasDirtyPred = true;
                    break;
                }
            }
            if (!hasDirtyPred)
                continue;
            BlockVarKey key = std::make_pair(block, V);
            s_[key] =
                model_->addVar(0.0, 1.0, 0.0, GRB_CONTINUOUS, makeVarName(cfg_, "s", block, V));
            sKeys_.push_back(key);
        }
    }

    // eAccum_[b] for all blocks.
    for (NodeId block : cfg_.getBlocks()) {
        eAccum_[block] =
            model_->addVar(0.0, Ebuf, 0.0, GRB_CONTINUOUS, "eAccum_" + nodeToken(cfg_, block));
    }

    model_->update();
}

void CheckpointOptimizer::addObjective() {
    GRBLinExpr objective = 0;
    NodeId entry = cfg_.getEntryBlock();

    // Term 1: placement penalty (eligible only).
    for (NodeId block : cfg_.getBlocks()) {
        double fEntry = energy_.getFEntry(block);
        for (llvm::GlobalVariable *GV : orderedVmObjs_) {
            double eNvm = energy_.getENvm(block, GV);
            if (eNvm == 0.0)
                continue;
            objective += fEntry * eNvm;
            if (coarseAllocation_) {
                objective -= fEntry * eNvm * mGlobal_.at(GV);
            } else {
                objective -=
                    fEntry * eNvm * m_.at(std::make_pair(block, static_cast<llvm::Value *>(GV)));
            }
        }
    }

    // Term 2: expected region-start overhead.
    // Use fBoundary (= preheader freq for summary nodes) so boundary costs
    // are weighted by loop entry frequency, not per-iteration frequency.
    for (NodeId block : cfg_.getBlocks()) {
        objective += energy_.getFBoundary(block) * buildEStart(block);
    }

    // Term 3: expected region-end overhead (excluding entry).
    for (NodeId block : cfg_.getBlocks()) {
        if (block == entry)
            continue;
        objective += energy_.getFBoundary(block) * buildEEnd(block);
    }

    model_->setObjective(objective, GRB_MINIMIZE);
}

void CheckpointOptimizer::addConstraints() {
    constrainEntryAsRegionStart();       // C1
    constrainIneligiblePlacement();      // C2
    constrainNeedRestoreLinearization(); // C3
    constrainVMCapacity();               // VM capacity
    constrainPlacementPropagation();     // C12, C13
    constrainDirtyPropagation();         // C4, C5, C6
    constrainSaveAtRegionBoundary();     // C8 (dHat eliminated)
    constrainEnergyInitAtRegionStart();  // C9
    constrainEnergyPropagation();        // C10
    constrainEnergyWithinCapacity();     // C11
}

// C1: r[entry] = 1
void CheckpointOptimizer::constrainEntryAsRegionStart() {
    NodeId entry = cfg_.getEntryBlock();
    if (entry != kInvalidNodeId && r_.count(entry)) {
        model_->addConstr(r_[entry] == 1, "C1_entry_region_start");
    }
}

// C2: Ineligible variables have m=1 (not modeled as Gurobi variables).
// No constraints needed — handled by not creating m[b,v] for ineligibles
// and substituting constant 1 wherever m would appear.
void CheckpointOptimizer::constrainIneligiblePlacement() {
    // Intentionally empty: m[b,v] for ineligibles is not a Gurobi variable.
}

// Σ size(v) * m[b,v] <= VM_capacity  (∀ b)
// Ineligible variables have m=1 (constant), moved to RHS as reduced capacity.
void CheckpointOptimizer::constrainVMCapacity() {
    // Compute fixed VM usage from ineligible variables (m=1 always).
    double ineligibleUsage = 0.0;
    for (llvm::Value *V : orderedTrackedValues_) {
        if (!state_.isIneligible(V))
            continue;
        if (!llvm::isa<llvm::GlobalVariable>(V) && !llvm::isa<llvm::AllocaInst>(V))
            continue;
        int sizeBytes = state_.getVarSizeBytes(V);
        if (sizeBytes > 0)
            ineligibleUsage += static_cast<double>(sizeBytes);
    }
    double reducedCapacity = static_cast<double>(params_.vmCapacityBytes) - ineligibleUsage;

    if (coarseAllocation_) {
        GRBLinExpr vmUsage = 0;
        for (llvm::GlobalVariable *GV : orderedVmObjs_) {
            int sizeBytes = state_.getVarSizeBytes(GV);
            if (sizeBytes <= 0)
                continue;
            vmUsage += static_cast<double>(sizeBytes) * mGlobal_.at(GV);
        }
        model_->addConstr(vmUsage <= reducedCapacity, "vm_capacity_global");
        return;
    }

    for (NodeId block : cfg_.getBlocks()) {
        GRBLinExpr vmUsage = 0;
        for (llvm::GlobalVariable *GV : orderedVmObjs_) {
            int sizeBytes = state_.getVarSizeBytes(GV);
            if (sizeBytes <= 0)
                continue;
            vmUsage += static_cast<double>(sizeBytes) *
                       m_.at(std::make_pair(block, static_cast<llvm::Value *>(GV)));
        }
        model_->addConstr(vmUsage <= reducedCapacity, "vm_capacity_" + nodeToken(cfg_, block));
    }
}

// C3: rHat[b,v] = r[b] AND m[b,v]  (only for eligible + live-in pairs)
// McCormick linearization (exact when r,m are binary):
//   rHat <= r,  rHat <= m,  rHat >= r + m - 1
void CheckpointOptimizer::constrainNeedRestoreLinearization() {
    for (const BlockVarKey &key : rHatKeys_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;
        GRBVar rHatVar = rHat_.at(key);
        auto *GV = llvm::cast<llvm::GlobalVariable>(V);
        std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);
        model_->addConstr(rHatVar <= r_[block], "C3_rHat_le_r_" + suffix);
        if (coarseAllocation_) {
            model_->addConstr(rHatVar <= mGlobal_.at(GV), "C3_rHat_le_m_" + suffix);
            model_->addConstr(rHatVar >= r_[block] + mGlobal_.at(GV) - 1,
                              "C3_rHat_ge_rm_" + suffix);
        } else {
            model_->addConstr(rHatVar <= m_.at(key), "C3_rHat_le_m_" + suffix);
            model_->addConstr(rHatVar >= r_[block] + m_.at(key) - 1, "C3_rHat_ge_rm_" + suffix);
        }
    }
}

// C12: m[succ,v] <= m[pred,v] + r[succ]  (∀ edge, v ∈ V_elig)
// C13: m[succ,v] >= m[pred,v] - r[succ]  (∀ edge, v ∈ V_elig)
void CheckpointOptimizer::constrainPlacementPropagation() {
    if (coarseAllocation_)
        return;

    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::GlobalVariable *GV : orderedVmObjs_) {
            BlockVarKey predKey = std::make_pair(pred, static_cast<llvm::Value *>(GV));
            BlockVarKey succKey = std::make_pair(succ, static_cast<llvm::Value *>(GV));
            model_->addConstr(m_[succKey] <= m_[predKey] + r_[succ],
                              "C12_placement_fwd_" + std::to_string(idx));
            model_->addConstr(m_[succKey] >= m_[predKey] - r_[succ],
                              "C13_placement_bwd_" + std::to_string(idx));
            idx++;
        }
    }

    // At merge points, force consistent placement across all predecessors:
    // m[pred,v] = m[pred',v] for every pair of predecessors of a merge block.
    unsigned mergeIdx = 0;
    for (NodeId block : cfg_.getBlocks()) {
        if (block == cfg_.getEntryBlock())
            continue;
        const auto &preds = predecessors_[block];
        if (preds.size() <= 1)
            continue;
        for (llvm::GlobalVariable *GV : orderedVmObjs_) {
            auto *V = static_cast<llvm::Value *>(GV);
            // Constrain all predecessors to match the first predecessor's placement.
            NodeId firstPred = preds[0];
            BlockVarKey firstKey = std::make_pair(firstPred, V);
            for (size_t i = 1; i < preds.size(); ++i) {
                BlockVarKey otherKey = std::make_pair(preds[i], V);
                model_->addConstr(m_[firstKey] == m_[otherKey],
                                  "merge_placement_eq_" + std::to_string(mergeIdx));
                mergeIdx++;
            }
        }
    }
}

// C4:  d[b,v] >= D_{b,v}                              (∀ b ∈ Reach(v), v)
// C5:  d[succ,v] >= d[pred,v] - r[succ]               (∀ edge, v where both in Reach(v))
// C5': d[b,v] <= D_{b,v} + Σ_pred d[pred,v]           (∀ b ∈ Reach(v), v)  [LP tightening]
// C6:  d[b,v] <= D_{b,v} - r[b] + 1                   (∀ b ∈ Reach(v), v)
// d[b,v] is not created for b ∉ Reach(v) (provably 0).
void CheckpointOptimizer::constrainDirtyPropagation() {
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : orderedTrackedValues_) {
            BlockVarKey key = std::make_pair(block, V);
            auto dIt = d_.find(key);
            if (dIt == d_.end())
                continue; // b ∉ Reach(v), d[b,v] = 0

            GRBVar dVar = dIt->second;

            bool isDef = false;
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
                isDef = state_.getEligDefIndicator(block, GV);
            if (!isDef)
                isDef = state_.getIneligDefIndicator(block, V);
            double def = isDef ? 1.0 : 0.0;

            std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);

            // C4: d[b,v] >= D_{b,v}
            model_->addConstr(dVar >= def, "C4_dirty_local_" + suffix);

            // C5': d[b,v] <= D_{b,v} + Σ_pred d[pred,v]  [LP tightening]
            // Predecessors outside Reach(v) contribute 0.
            GRBLinExpr predSum = 0;
            for (NodeId pred : predecessors_[block]) {
                auto predIt = d_.find(std::make_pair(pred, V));
                if (predIt != d_.end())
                    predSum += predIt->second;
            }
            model_->addConstr(dVar <= def + predSum, "C5p_dirty_upper_" + suffix);

            // C6: d[b,v] <= D_{b,v} - r[b] + 1
            model_->addConstr(dVar <= def + (1 - r_[block]), "C6_dirty_reset_" + suffix);
        }
    }

    // C5: d[succ,v] >= d[pred,v] - r[succ]  (∀ edge, v where both in Reach(v))
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::Value *V : orderedTrackedValues_) {
            auto predIt = d_.find(std::make_pair(pred, V));
            auto succIt = d_.find(std::make_pair(succ, V));
            if (predIt == d_.end() || succIt == d_.end())
                continue; // One or both outside Reach(v), constraint is trivial.
            model_->addConstr(succIt->second >= predIt->second - r_[succ],
                              "C5_dirty_edge_" + std::to_string(idx));
            idx++;
        }
    }
}

// C8:  s[succ,v] >= d[pred,v] + m[pred,v] + r[succ] - 2   (∀ edge, v)
//      (eliminates dHat by substituting dHat >= d + m - 1)
// C8': s[b,v] <= r[b]                                      (∀ b, v)  [LP tightening]
// C8': s[b,v] <= Σ_pred d[pred,v]                           (∀ b, v)  [LP tightening]
// C8': s[b,v] <= Σ_pred m[pred,v]                           (∀ b, v)  [LP tightening]
void CheckpointOptimizer::constrainSaveAtRegionBoundary() {
    // LP tightening for s variables.
    for (const BlockVarKey &key : sKeys_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;
        GRBVar sVar = s_.at(key);
        std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);

        // C8': s[b,v] <= r[b]
        model_->addConstr(sVar <= r_[block], "C8p_save_le_r_" + suffix);

        // C8': s[b,v] <= Σ_pred d[pred,v]  (replaces dHat upper bound)
        // Predecessors outside Reach(v) contribute 0.
        GRBLinExpr predDirtySum = 0;
        for (NodeId pred : predecessors_[block]) {
            auto predIt = d_.find(std::make_pair(pred, V));
            if (predIt != d_.end())
                predDirtySum += predIt->second;
        }
        model_->addConstr(sVar <= predDirtySum, "C8p_save_le_dirty_" + suffix);

        // C8': s[b,v] <= Σ_pred m[pred,v]  (replaces dHat upper bound)
        // For ineligibles, m=1 so s <= |preds|, trivially satisfied — skip.
        if (!state_.isIneligible(V)) {
            auto *GV = llvm::cast<llvm::GlobalVariable>(V);
            if (coarseAllocation_) {
                model_->addConstr(sVar <= mGlobal_.at(GV), "C8p_save_le_place_" + suffix);
            } else {
                GRBLinExpr predPlaceSum = 0;
                for (NodeId pred : predecessors_[block]) {
                    predPlaceSum += m_.at(std::make_pair(pred, V));
                }
                model_->addConstr(sVar <= predPlaceSum, "C8p_save_le_place_" + suffix);
            }
        }
    }

    // C8: s[succ,v] >= d[pred,v] + m[pred,v] + r[succ] - 2  (∀ edge, v where s exists)
    // Build per-block save variable lookup for efficiency.
    std::map<NodeId, std::vector<std::pair<llvm::Value *, GRBVar>>> saveByBlock;
    for (const BlockVarKey &key : sKeys_) {
        saveByBlock[key.first].emplace_back(key.second, s_.at(key));
    }

    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        auto it = saveByBlock.find(succ);
        if (it == saveByBlock.end())
            continue;
        for (const auto &[V, sVar] : it->second) {
            BlockVarKey predKey = std::make_pair(pred, V);
            auto dIt = d_.find(predKey);
            if (dIt == d_.end())
                continue; // d[pred,v] = 0, so RHS <= 0, trivially satisfied.
            if (state_.isIneligible(V)) {
                // m=1: s >= d + 1 + r - 2 = d + r - 1
                model_->addConstr(sVar >= dIt->second + r_[succ] - 1,
                                  "C8_save_edge_" + std::to_string(idx));
            } else {
                auto *GV = llvm::cast<llvm::GlobalVariable>(V);
                if (coarseAllocation_) {
                    model_->addConstr(sVar >= dIt->second + mGlobal_.at(GV) + r_[succ] - 2,
                                      "C8_save_edge_" + std::to_string(idx));
                } else {
                    model_->addConstr(sVar >= dIt->second + m_.at(predKey) + r_[succ] - 2,
                                      "C8_save_edge_" + std::to_string(idx));
                }
            }
            idx++;
        }
    }
}

// At region starts, accumulated energy equals the startup cost:
//   r[b]=1  →  eAccum[b] = E_start(b)
// Big-M reformulation (M = capacity, tight since eAccum ∈ [0, capacity]):
//   eAccum[b] >= E_start(b) - capacity * (1 - r[b])
//   eAccum[b] <= E_start(b) + capacity * (1 - r[b])
void CheckpointOptimizer::constrainEnergyInitAtRegionStart() {
    const double M = params_.capacity;
    for (NodeId block : cfg_.getBlocks()) {
        GRBLinExpr eStart = buildEStart(block);
        std::string suffix = nodeToken(cfg_, block);
        model_->addConstr(eAccum_[block] >= eStart - M * (1 - r_[block]),
                          "C9_energy_init_lb_" + suffix);
        model_->addConstr(eAccum_[block] <= eStart + M * (1 - r_[block]),
                          "C9_energy_init_ub_" + suffix);
    }
}

// Energy propagation along edges when not starting a new region:
//   r[dst]=0  →  eAccum[dst] >= eAccum[src] + E_blk(src)
// Big-M reformulation (M = capacity, tight since eAccum ∈ [0, capacity]):
//   eAccum[dst] >= eAccum[src] + E_blk(src) - capacity * r[dst]
void CheckpointOptimizer::constrainEnergyPropagation() {
    const double M = params_.capacity;
    unsigned edgeIdx = 0;
    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        model_->addConstr(eAccum_[dst] >= eAccum_[src] + eBlkSrc - M * r_[dst],
                          "C10_energy_prop_" + std::to_string(edgeIdx++));
    }
}

// Energy must not exceed buffer capacity on any path:
//   eAccum_[src] + E_blk(src) + E_end(dst) <= capacity   (per edge)
//   eAccum_[exit] + E_blk(exit) <= capacity               (at exits)
// Ensures every region can complete before the energy buffer is exhausted.
void CheckpointOptimizer::constrainEnergyWithinCapacity() {
    const double Ebuf = params_.capacity;
    unsigned edgeIdx = 0;

    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        GRBLinExpr eEndDst = buildEEnd(dst);
        model_->addConstr(eAccum_[src] + eBlkSrc + eEndDst <= Ebuf,
                          "C11_energy_edge_" + std::to_string(edgeIdx++));
    }

    for (NodeId exitBlock : cfg_.getExitBlocks()) {
        GRBLinExpr eBlkExit = buildEBlk(exitBlock);
        model_->addConstr(eAccum_[exitBlock] + eBlkExit <= Ebuf,
                          "C11_energy_exit_" + nodeToken(cfg_, exitBlock));
    }
}

GRBLinExpr CheckpointOptimizer::buildEBlk(NodeId block) {
    GRBLinExpr expr = energy_.getEBase(block);
    for (llvm::GlobalVariable *GV : orderedVmObjs_) {
        double eNvm = energy_.getENvm(block, GV);
        if (eNvm == 0.0)
            continue;
        expr += eNvm;
        if (coarseAllocation_) {
            expr -= eNvm * mGlobal_.at(GV);
        } else {
            expr -= eNvm * m_.at(std::make_pair(block, static_cast<llvm::Value *>(GV)));
        }
    }
    return expr;
}

// E_start(b) = E_pro * r[b] + Σ_v E_rst_v * rHat[b,v]
// rHat_ only contains eligible+live-in entries.
// Ineligible+live-in: rHat = r[b], so contribute E_rst_v * r[b].
GRBLinExpr CheckpointOptimizer::buildEStart(NodeId block) {
    GRBLinExpr expr = params_.E_pro * r_[block];

    // Eligible + live-in: use rHat variable.
    for (const BlockVarKey &key : rHatKeys_) {
        if (key.first != block)
            continue;
        GRBVar rHatVar = rHat_.at(key);
        double eRestore = energy_.getERestore(key.second);
        if (eRestore > 0.0) {
            expr += eRestore * rHatVar;
        }
    }

    // Ineligible + live-in: rHat = r[b], so E_rst_v * r[b].
    // Sorted iteration keeps floating-point coefficient accumulation
    // deterministic across runs (pointer-ordered set iteration is not).
    for (llvm::Value *V : collectSortedValues(state_.getIneligLiveIn(block))) {
        double eRestore = energy_.getERestore(V);
        if (eRestore > 0.0) {
            expr += eRestore * r_[block];
        }
    }

    return expr;
}

GRBLinExpr CheckpointOptimizer::buildEEnd(NodeId block) {
    GRBLinExpr expr = 0;
    expr += params_.E_epi * r_[block];

    for (const BlockVarKey &key : sKeys_) {
        if (key.first != block)
            continue;
        GRBVar saveVar = s_.at(key);
        double eSave = energy_.getESave(key.second);
        if (eSave > 0.0) {
            expr += eSave * saveVar;
        }
    }

    return expr;
}

void CheckpointOptimizer::extractSolution() {
    // Reset cached containers so repeated extraction cannot leave stale
    // block-expanded coarse-allocation entries in the instrumenter view.
    solution_.r.clear();
    solution_.m.clear();
    solution_.rHat.clear();
    solution_.s.clear();
    solution_.eAccum.clear();

    for (const auto &[block, var] : r_) {
        if (var.get(GRB_DoubleAttr_X) > 0.5) {
            solution_.r.insert(block);
        }
    }

    if (coarseAllocation_) {
        for (llvm::GlobalVariable *GV : orderedVmObjs_) {
            bool placed = mGlobal_.at(GV).get(GRB_DoubleAttr_X) > 0.5;
            for (NodeId block : cfg_.getBlocks()) {
                solution_.m[std::make_pair(block, static_cast<llvm::Value *>(GV))] = placed;
            }
        }
    } else {
        for (const auto &[key, var] : m_) {
            solution_.m[key] = var.get(GRB_DoubleAttr_X) > 0.5;
        }
    }
    // Ineligible variables have m=1 (not modeled as Gurobi variables).
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : state_.getIneligibleObjs()) {
            solution_.m[std::make_pair(block, V)] = true;
        }
    }

    for (const auto &[key, var] : rHat_) {
        solution_.rHat[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }
    // Ineligible + live-in: rHat = r[b].
    for (NodeId block : cfg_.getBlocks()) {
        bool isRegionStart = solution_.r.count(block) > 0;
        for (llvm::Value *V : state_.getIneligLiveIn(block)) {
            solution_.rHat[std::make_pair(block, V)] = isRegionStart;
        }
    }

    for (const auto &[key, var] : s_) {
        solution_.s[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    for (const auto &[block, var] : eAccum_) {
        solution_.eAccum[block] = var.get(GRB_DoubleAttr_X);
    }

    solution_.objectiveValue = model_->get(GRB_DoubleAttr_ObjVal);
}

} // namespace checkpoint
