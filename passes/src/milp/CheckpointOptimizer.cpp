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

} // namespace

CheckpointOptimizer::CheckpointOptimizer(const MILPInput &input)
    : cfg_(input.cfg), state_(input.state), energy_(input.energy),
      params_(input.energy.getParams()), env_(), model_(env_) {
    model_.set(GRB_IntParam_OutputFlag, 0);
}

int CheckpointOptimizer::getNumVars() const {
    return model_.get(GRB_IntAttr_NumVars);
}

int CheckpointOptimizer::getNumConstrs() const {
    return model_.get(GRB_IntAttr_NumConstrs);
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

    buildModel();
    if (timeLimit_ > 0.0)
        model_.set(GRB_DoubleParam_TimeLimit, timeLimit_);
    model_.optimize();
    solved_ = true;

    int status = model_.get(GRB_IntAttr_Status);
    if (status == GRB_OPTIMAL) {
        extractSolution();
        solution_.solverStatus = SolverStatus::Optimal;
        return true;
    }

    int solCount = model_.get(GRB_IntAttr_SolCount);
    PLOGE << "Optimization did not prove optimality"
          << " (Gurobi status=" << status << ", solutions found=" << solCount << ")";

    if (status == GRB_INFEASIBLE) {
        PLOGE << "Optimization infeasible; computing IIS diagnostics...";
        model_.computeIIS();
        model_.write("milp_infeasible.lp");
        model_.write("milp_infeasible.ilp");
        PLOGD << "  Wrote model: milp_infeasible.lp";
        PLOGD << "  Wrote IIS:   milp_infeasible.ilp";

        auto constrs = model_.getConstrs();
        int numConstrs = model_.get(GRB_IntAttr_NumConstrs);
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

        auto vars = model_.getVars();
        int numVars = model_.get(GRB_IntAttr_NumVars);
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
        double gap = model_.get(GRB_DoubleAttr_MIPGap);
        PLOGW << "Accepting feasible solution (MIP gap=" << gap << ")";
        extractSolution();
        solution_.solverStatus = SolverStatus::Feasible;
        solution_.mipGap = gap;
        return true;
    }

    return false;
}

void CheckpointOptimizer::buildModel() {
    addVariables();
    addObjective();
    predecessors_ = buildPredecessorMap(cfg_);
    addConstraints();
    model_.update();
}

void CheckpointOptimizer::addVariables() {
    const double Ebuf = params_.capacity;
    NodeId entry = cfg_.getEntryBlock();

    // r_b for all blocks.
    for (NodeId block : cfg_.getBlocks()) {
        r_[block] = model_.addVar(0.0, 1.0, 0.0, GRB_BINARY, "r_" + nodeToken(cfg_, block));
    }

    // Build combined list of all tracked variables as Value*.
    std::vector<llvm::Value *> allTracked;
    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        allTracked.push_back(static_cast<llvm::Value *>(GV));
    allTracked.insert(allTracked.end(), state_.getIneligibleObjs().begin(),
                      state_.getIneligibleObjs().end());

    // m_{b,v}, d_{b,v}, dHat_{b,v} for all (block, var).
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : allTracked) {
            BlockVarKey key = std::make_pair(block, V);
            m_[key] = model_.addVar(0.0, 1.0, 0.0, GRB_BINARY, makeVarName(cfg_, "m", block, V));
            d_[key] = model_.addVar(0.0, 1.0, 0.0, GRB_BINARY, makeVarName(cfg_, "d", block, V));
            dHat_[key] =
                model_.addVar(0.0, 1.0, 0.0, GRB_CONTINUOUS, makeVarName(cfg_, "dHat", block, V));
        }
    }

    // rHat_{b,v} for all (block, var).
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : allTracked) {
            BlockVarKey key = std::make_pair(block, V);
            rHat_[key] =
                model_.addVar(0.0, 1.0, 0.0, GRB_CONTINUOUS, makeVarName(cfg_, "rHat", block, V));
        }
    }

    // s_{b,v} for b != entry, v in LiveIn(b).
    for (NodeId block : cfg_.getBlocks()) {
        if (block == entry)
            continue;
        // Union of eligible and ineligible live-ins.
        std::set<llvm::Value *> liveIn;
        for (llvm::GlobalVariable *GV : state_.getEligLiveIn(block))
            liveIn.insert(static_cast<llvm::Value *>(GV));
        for (llvm::Value *V : state_.getIneligLiveIn(block))
            liveIn.insert(V);
        for (llvm::Value *V : liveIn) {
            BlockVarKey key = std::make_pair(block, V);
            s_[key] =
                model_.addVar(0.0, 1.0, 0.0, GRB_CONTINUOUS, makeVarName(cfg_, "s", block, V));
        }
    }

    // eAccum_[b] for all blocks.
    for (NodeId block : cfg_.getBlocks()) {
        eAccum_[block] =
            model_.addVar(0.0, Ebuf, 0.0, GRB_CONTINUOUS, "eAccum_" + nodeToken(cfg_, block));
    }

    model_.update();
}

void CheckpointOptimizer::addObjective() {
    GRBLinExpr objective = 0;
    NodeId entry = cfg_.getEntryBlock();

    // Term 1: placement penalty (eligible only).
    for (NodeId block : cfg_.getBlocks()) {
        double fEntry = energy_.getFEntry(block);
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            double eNvm = energy_.getENvm(block, GV);
            if (eNvm == 0.0)
                continue;
            objective += fEntry * eNvm;
            objective -= fEntry * eNvm * m_[std::make_pair(block, static_cast<llvm::Value *>(GV))];
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

    model_.setObjective(objective, GRB_MINIMIZE);
}

void CheckpointOptimizer::addConstraints() {
    constrainEntryAsRegionStart();       // C1
    constrainIneligiblePlacement();      // C2
    constrainNeedRestoreLinearization(); // C3
    constrainVMCapacity();               // VM capacity
    constrainPlacementPropagation();     // C12, C13
    constrainDirtyPropagation();         // C4, C5, C6
    constrainSaveAtRegionBoundary();     // C7, C8
    constrainEnergyInitAtRegionStart();  // C9
    constrainEnergyPropagation();        // C10
    constrainEnergyWithinCapacity();     // C11
}

// Entry block is always a region start:
//   r_[entry] = 1
void CheckpointOptimizer::constrainEntryAsRegionStart() {
    NodeId entry = cfg_.getEntryBlock();
    if (entry != kInvalidNodeId && r_.count(entry)) {
        model_.addConstr(r_[entry] == 1, "entry_region_start");
    }
}

// C2: m[b,v] = 1 for ineligible variables (always in VM).
void CheckpointOptimizer::constrainIneligiblePlacement() {
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : state_.getIneligibleObjs()) {
            BlockVarKey key = std::make_pair(block, V);
            model_.addConstr(m_[key] == 1,
                             "inelig_m_" + nodeToken(cfg_, block) + "_" + valueToken(V));
        }
    }
}

// Per-block VM (SRAM) capacity limit:
//   sum_v( size(v) * m[b,v] ) + ineligibleSize <= VM_capacity
// Eligible objects may or may not be in VM; ineligible memory objects always are.
void CheckpointOptimizer::constrainVMCapacity() {
    if (state_.getVMObjs().empty() && state_.getIneligibleObjs().empty())
        return;

    // Memory-backed ineligible objects always occupy VM (e.g., globals/allocas).
    // Cross-block SSA values are checkpoint-tracked state but do not consume
    // persistent VM capacity.
    double ineligibleSize = 0;
    for (llvm::Value *V : state_.getIneligibleObjs()) {
        if (!llvm::isa<llvm::GlobalVariable>(V) && !llvm::isa<llvm::AllocaInst>(V)) {
            continue;
        }
        int sizeBytes = state_.getVarSizeBytes(V);
        if (sizeBytes > 0)
            ineligibleSize += static_cast<double>(sizeBytes);
    }

    for (NodeId block : cfg_.getBlocks()) {
        GRBLinExpr vmUsage = 0;
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            int sizeBytes = state_.getVarSizeBytes(GV);
            if (sizeBytes <= 0) {
                continue;
            }
            vmUsage += static_cast<double>(sizeBytes) *
                       m_[std::make_pair(block, static_cast<llvm::Value *>(GV))];
        }
        model_.addConstr(vmUsage + ineligibleSize <= static_cast<double>(params_.vmCapacityBytes),
                         "vm_capacity_" + nodeToken(cfg_, block));
    }
}

// rHat[b,v] = r_[b] AND m[b,v]
// A variable needs restoring from FRAM only when a new region begins
// and the variable is placed in volatile memory (VM/SRAM).
void CheckpointOptimizer::constrainNeedRestoreLinearization() {
    for (const auto &[key, needVar] : rHat_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;
        auto placeIt = m_.find(key);
        if (placeIt == m_.end())
            continue;

        std::string prefix = "rHat_" + nodeToken(cfg_, block) + "_" + valueToken(V);
        model_.addConstr(needVar <= r_[block], prefix + "_le_r");
        model_.addConstr(needVar <= placeIt->second, prefix + "_le_m");
        model_.addConstr(needVar >= r_[block] + placeIt->second - 1, prefix + "_ge_rm");
    }
}

// Placement consistency across CFG edges:
//   m[succ,v] <= m[pred,v] + r_[succ]
//   m[succ,v] >= m[pred,v] - r_[succ]
// Within a region (r_=0), placement is inherited from predecessors.
// At region boundaries, placement may change freely.
void CheckpointOptimizer::constrainPlacementPropagation() {
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockVarKey predKey = std::make_pair(pred, static_cast<llvm::Value *>(GV));
            BlockVarKey succKey = std::make_pair(succ, static_cast<llvm::Value *>(GV));
            GRBVar pPred = m_[predKey];
            GRBVar pSucc = m_[succKey];
            GRBVar xSucc = r_[succ];
            model_.addConstr(pSucc <= pPred + xSucc, "placement_prop_fwd_" + std::to_string(idx));
            model_.addConstr(pSucc >= pPred - xSucc, "placement_prop_bwd_" + std::to_string(idx));
            idx++;
        }
    }

    // Forbid region boundaries at merge points.  EdgeSplitPass guarantees
    // that every predecessor of a merge point has a single predecessor,
    // so the optimizer will place boundaries at those split blocks instead.
    for (NodeId block : cfg_.getBlocks()) {
        if (block == cfg_.getEntryBlock())
            continue;
        const auto &preds = predecessors_[block];
        if (preds.size() > 1) {
            model_.addConstr(r_[block] == 0, "no_boundary_merge_" + std::to_string(block));
        }
    }
}

// Dirty-state propagation for all tracked variables (eligible + ineligible):
//   d[b,v] >= def(b,v)                                       (local def)
//   d[b,v] <= def(b,v) + sum_{p in preds(b)} d[p,v]         (upper bound)
//   d[b,v] <= def(b,v) + (1 - r_[b])                        (region reset)
//   d[succ,v] >= d[pred,v] - r_[succ]                       (edge prop)
// Tracks whether variable v has an uncommitted modification reaching block b.
// Region starts reset dirty state (forcing a save at the boundary).
void CheckpointOptimizer::constrainDirtyPropagation() {
    // Build combined list of all tracked variables as Value* (elig + inelig).
    std::vector<llvm::Value *> allTracked;
    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        allTracked.push_back(static_cast<llvm::Value *>(GV));
    allTracked.insert(allTracked.end(), state_.getIneligibleObjs().begin(),
                      state_.getIneligibleObjs().end());

    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : allTracked) {
            BlockVarKey key = std::make_pair(block, V);
            GRBVar p = d_[key];

            // Def indicator: check eligible then ineligible.
            bool isDef = false;
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
                isDef = state_.getEligDefIndicator(block, GV);
            if (!isDef)
                isDef = state_.getIneligDefIndicator(block, V);
            double def = isDef ? 1.0 : 0.0;

            std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);
            model_.addConstr(p >= def, "d_local_def_" + suffix);

            GRBLinExpr predSum = 0;
            for (NodeId pred : predecessors_[block]) {
                predSum += d_[std::make_pair(pred, V)];
            }
            model_.addConstr(p <= def + predSum, "d_upper_bound_" + suffix);
            model_.addConstr(p <= def + (1 - r_[block]), "d_region_reset_" + suffix);
        }
    }

    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::Value *V : allTracked) {
            model_.addConstr(d_[std::make_pair(succ, V)] >= d_[std::make_pair(pred, V)] - r_[succ],
                             "d_edge_prop_" + std::to_string(idx));
            idx++;
        }
    }
}

// Save model — forces dirty state to be saved at region boundaries.
//
// dHat[b,v] = d[b,v] AND m[b,v]  (eligible only)
//   Tracks dirty modifications for VM-placed eligible variables.
//
// For each save variable (b != entry, v in LiveIn(b)):
//   s[b,v] <= r_[b]                          (only at boundaries)
//   s[b,v] <= sum_{p} state[p,v]             (some pred must be dirty)
//   s[b,v] >= r_[b] + state[p,v] - 1        (for each pred p)
// where state = d (ineligible) or dHat (eligible).
void CheckpointOptimizer::constrainSaveAtRegionBoundary() {
    // dHat[b,v] = d[b,v] AND m[b,v] — eligibles only
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockVarKey key = std::make_pair(block, static_cast<llvm::Value *>(GV));
            std::string prefix =
                "dHat_and_" + nodeToken(cfg_, block) + "_" + sanitizeToken(GV->getName());
            model_.addConstr(dHat_[key] <= d_[key], prefix + "_le_d");
            model_.addConstr(dHat_[key] <= m_[key], prefix + "_le_m");
            model_.addConstr(dHat_[key] >= d_[key] + m_[key] - 1, prefix + "_ge_dm");
        }
    }

    // s[b,v] model (for b != entry and v in LiveIn(b))
    for (const auto &[key, saveVar] : s_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;

        std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);

        model_.addConstr(saveVar <= r_[block], "s_le_r_" + suffix);

        if (state_.isIneligible(V)) {
            // Ineligible save — uses d directly (always in VM).
            GRBLinExpr predDirty = 0;
            for (NodeId pred : predecessors_[block]) {
                predDirty += d_[std::make_pair(pred, V)];
            }
            model_.addConstr(saveVar <= predDirty, "inelig_s_le_preds_" + suffix);
            for (NodeId pred : predecessors_[block]) {
                model_.addConstr(saveVar >= r_[block] + d_[std::make_pair(pred, V)] - 1,
                                 "inelig_s_ge_pred_" + nodeToken(cfg_, pred) + "_" + suffix);
            }
        } else {
            // Eligible save — uses dHat.
            auto *GV = llvm::cast<llvm::GlobalVariable>(V);
            GRBLinExpr predDHat = 0;
            for (NodeId pred : predecessors_[block]) {
                predDHat += dHat_[std::make_pair(pred, static_cast<llvm::Value *>(GV))];
            }
            model_.addConstr(saveVar <= predDHat, "elig_s_le_preds_" + suffix);
            for (NodeId pred : predecessors_[block]) {
                model_.addConstr(
                    saveVar >=
                        r_[block] + dHat_[std::make_pair(pred, static_cast<llvm::Value *>(GV))] - 1,
                    "elig_s_ge_pred_" + nodeToken(cfg_, pred) + "_" + suffix);
            }
        }
    }
}

// At region starts, accumulated energy equals the startup cost:
// Indicator: r_[b] = 1  →  eAccum_[b] = E_start(b)
// Replaces big-M formulation with native Gurobi indicator constraints.
void CheckpointOptimizer::constrainEnergyInitAtRegionStart() {
    for (NodeId block : cfg_.getBlocks()) {
        GRBLinExpr eStart = buildEStart(block);
        model_.addGenConstrIndicator(r_[block], 1, eAccum_[block] - eStart, GRB_EQUAL, 0.0,
                                     "energy_init_" + nodeToken(cfg_, block));
    }
}

// Indicator: r_[dst] = 0  →  eAccum_[dst] >= eAccum_[src] + E_blk(src)
// Replaces big-M formulation with native Gurobi indicator constraints.
void CheckpointOptimizer::constrainEnergyPropagation() {
    unsigned edgeIdx = 0;
    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        model_.addGenConstrIndicator(r_[dst], 0, eAccum_[dst] - eAccum_[src] - eBlkSrc,
                                     GRB_GREATER_EQUAL, 0.0,
                                     "energy_propagation_" + std::to_string(edgeIdx++));
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
        model_.addConstr(eAccum_[src] + eBlkSrc + eEndDst <= Ebuf,
                         "energy_capacity_edge_" + std::to_string(edgeIdx++));
    }

    for (NodeId exitBlock : cfg_.getExitBlocks()) {
        GRBLinExpr eBlkExit = buildEBlk(exitBlock);
        model_.addConstr(eAccum_[exitBlock] + eBlkExit <= Ebuf,
                         "energy_capacity_exit_" + nodeToken(cfg_, exitBlock));
    }
}

GRBLinExpr CheckpointOptimizer::buildEBlk(NodeId block) {
    GRBLinExpr expr = energy_.getEBase(block);
    for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
        double eNvm = energy_.getENvm(block, GV);
        if (eNvm == 0.0)
            continue;
        expr += eNvm;
        expr -= eNvm * m_[std::make_pair(block, static_cast<llvm::Value *>(GV))];
    }
    return expr;
}

GRBLinExpr CheckpointOptimizer::buildEStart(NodeId block) {
    GRBLinExpr expr = 0;

    // Ineligible restore cost: unconditional at region start (constant coeff).
    double ineligRestoreCost = 0;
    for (llvm::Value *V : state_.getIneligLiveIn(block)) {
        double eRestore = energy_.getERestore(V);
        if (eRestore > 0.0)
            ineligRestoreCost += eRestore;
    }
    expr += (params_.E_pro + ineligRestoreCost) * r_[block];

    // Eligible restore cost: variable (depends on rHat).
    for (llvm::GlobalVariable *GV : state_.getEligLiveIn(block)) {
        BlockVarKey key = std::make_pair(block, static_cast<llvm::Value *>(GV));
        auto it = rHat_.find(key);
        if (it == rHat_.end())
            continue;
        double eRestore = energy_.getERestore(GV);
        if (eRestore > 0.0) {
            expr += eRestore * it->second;
        }
    }

    return expr;
}

GRBLinExpr CheckpointOptimizer::buildEEnd(NodeId block) {
    GRBLinExpr expr = 0;
    expr += params_.E_epi * r_[block];

    for (const auto &[key, saveVar] : s_) {
        if (key.first != block)
            continue;
        double eSave = energy_.getESave(key.second);
        if (eSave > 0.0) {
            expr += eSave * saveVar;
        }
    }

    return expr;
}

void CheckpointOptimizer::extractSolution() {
    for (const auto &[block, var] : r_) {
        if (var.get(GRB_DoubleAttr_X) > 0.5) {
            solution_.r.insert(block);
        }
    }

    for (const auto &[key, var] : m_) {
        solution_.m[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    for (const auto &[key, var] : rHat_) {
        solution_.rHat[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    for (const auto &[key, var] : s_) {
        solution_.s[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    for (const auto &[block, var] : eAccum_) {
        solution_.eAccum[block] = var.get(GRB_DoubleAttr_X);
    }

    solution_.objectiveValue = model_.get(GRB_DoubleAttr_ObjVal);
}

} // namespace checkpoint
