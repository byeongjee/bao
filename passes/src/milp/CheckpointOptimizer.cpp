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
                model_.addVar(0.0, 1.0, 0.0, GRB_BINARY, makeVarName(cfg_, "dHat", block, V));
        }
    }

    // rHat_{b,v} for all (block, var).
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : allTracked) {
            BlockVarKey key = std::make_pair(block, V);
            rHat_[key] =
                model_.addVar(0.0, 1.0, 0.0, GRB_BINARY, makeVarName(cfg_, "rHat", block, V));
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

// C1: r[entry] = 1
void CheckpointOptimizer::constrainEntryAsRegionStart() {
    NodeId entry = cfg_.getEntryBlock();
    if (entry != kInvalidNodeId && r_.count(entry)) {
        model_.addConstr(r_[entry] == 1, "C1_entry_region_start");
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

// Σ size(v) * m[b,v] <= VM_capacity  (∀ b)
void CheckpointOptimizer::constrainVMCapacity() {
    std::vector<llvm::Value *> allTracked;
    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        allTracked.push_back(static_cast<llvm::Value *>(GV));
    allTracked.insert(allTracked.end(), state_.getIneligibleObjs().begin(),
                      state_.getIneligibleObjs().end());

    for (NodeId block : cfg_.getBlocks()) {
        GRBLinExpr vmUsage = 0;
        for (llvm::Value *V : allTracked) {
            // Only memory-backed objects consume VM capacity.
            if (!llvm::isa<llvm::GlobalVariable>(V) && !llvm::isa<llvm::AllocaInst>(V))
                continue;
            int sizeBytes = state_.getVarSizeBytes(V);
            if (sizeBytes <= 0)
                continue;
            vmUsage += static_cast<double>(sizeBytes) * m_[std::make_pair(block, V)];
        }
        model_.addConstr(vmUsage <= static_cast<double>(params_.vmCapacityBytes),
                         "vm_capacity_" + nodeToken(cfg_, block));
    }
}

// C3: rHat[b,v] = r[b] AND m[b,v] AND L_{b,v}
//     When L_{b,v}=0: rHat[b,v] = 0
//     When L_{b,v}=1: rHat[b,v] = r[b] AND m[b,v]  (via addGenConstrAnd)
void CheckpointOptimizer::constrainNeedRestoreLinearization() {
    // Build live-in check.
    auto isLiveIn = [&](NodeId block, llvm::Value *V) -> bool {
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
            return state_.getEligLiveIn(block).count(GV) > 0;
        return state_.getIneligLiveIn(block).count(V) > 0;
    };

    for (const auto &[key, rHatVar] : rHat_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;

        if (!isLiveIn(block, V)) {
            // L_{b,v} = 0: fix rHat = 0.
            model_.addConstr(rHatVar == 0,
                             "C3_rHat_not_live_" + nodeToken(cfg_, block) + "_" + valueToken(V));
        } else {
            // L_{b,v} = 1: rHat = r AND m.
            GRBVar inputs[] = {r_[block], m_[key]};
            model_.addGenConstrAnd(rHatVar, inputs, 2,
                                   "C3_rHat_and_" + nodeToken(cfg_, block) + "_" + valueToken(V));
        }
    }
}

// C12: m[succ,v] <= m[pred,v] + r[succ]  (∀ edge, v ∈ V_elig)
// C13: m[succ,v] >= m[pred,v] - r[succ]  (∀ edge, v ∈ V_elig)
void CheckpointOptimizer::constrainPlacementPropagation() {
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockVarKey predKey = std::make_pair(pred, static_cast<llvm::Value *>(GV));
            BlockVarKey succKey = std::make_pair(succ, static_cast<llvm::Value *>(GV));
            model_.addConstr(m_[succKey] <= m_[predKey] + r_[succ],
                             "C12_placement_fwd_" + std::to_string(idx));
            model_.addConstr(m_[succKey] >= m_[predKey] - r_[succ],
                             "C13_placement_bwd_" + std::to_string(idx));
            idx++;
        }
    }

    // Forbid region boundaries at merge points.
    for (NodeId block : cfg_.getBlocks()) {
        if (block == cfg_.getEntryBlock())
            continue;
        const auto &preds = predecessors_[block];
        if (preds.size() > 1) {
            model_.addConstr(r_[block] == 0, "no_boundary_merge_" + std::to_string(block));
        }
    }
}

// C4:  d[b,v] >= D_{b,v}                              (∀ b, v)
// C5:  d[succ,v] >= d[pred,v] - r[succ]               (∀ edge, v)
// C5': d[b,v] <= D_{b,v} + Σ_pred d[pred,v]           (∀ b, v)  [LP tightening]
// C6:  d[b,v] <= D_{b,v} - r[b] + 1                   (∀ b, v)
void CheckpointOptimizer::constrainDirtyPropagation() {
    std::vector<llvm::Value *> allTracked;
    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        allTracked.push_back(static_cast<llvm::Value *>(GV));
    allTracked.insert(allTracked.end(), state_.getIneligibleObjs().begin(),
                      state_.getIneligibleObjs().end());

    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : allTracked) {
            BlockVarKey key = std::make_pair(block, V);
            GRBVar dVar = d_[key];

            bool isDef = false;
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
                isDef = state_.getEligDefIndicator(block, GV);
            if (!isDef)
                isDef = state_.getIneligDefIndicator(block, V);
            double def = isDef ? 1.0 : 0.0;

            std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);

            // C4: d[b,v] >= D_{b,v}
            model_.addConstr(dVar >= def, "C4_dirty_local_" + suffix);

            // C5': d[b,v] <= D_{b,v} + Σ_pred d[pred,v]  [LP tightening]
            GRBLinExpr predSum = 0;
            for (NodeId pred : predecessors_[block]) {
                predSum += d_[std::make_pair(pred, V)];
            }
            model_.addConstr(dVar <= def + predSum, "C5p_dirty_upper_" + suffix);

            // C6: d[b,v] <= D_{b,v} - r[b] + 1
            model_.addConstr(dVar <= def + (1 - r_[block]), "C6_dirty_reset_" + suffix);
        }
    }

    // C5: d[succ,v] >= d[pred,v] - r[succ]  (∀ edge, v)
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::Value *V : allTracked) {
            model_.addConstr(d_[std::make_pair(succ, V)] >= d_[std::make_pair(pred, V)] - r_[succ],
                             "C5_dirty_edge_" + std::to_string(idx));
            idx++;
        }
    }
}

// C7:  dHat[b,v] = d[b,v] AND m[b,v]                  (via addGenConstrAnd)
// C8:  s[succ,v] >= dHat[pred,v] + r[succ] - 1        (∀ edge, v)
// C8': s[b,v] <= r[b]                                  (∀ b, v)  [LP tightening]
// C8': s[b,v] <= Σ_pred dHat[pred,v]                   (∀ b, v)  [LP tightening]
void CheckpointOptimizer::constrainSaveAtRegionBoundary() {
    std::vector<llvm::Value *> allTracked;
    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        allTracked.push_back(static_cast<llvm::Value *>(GV));
    allTracked.insert(allTracked.end(), state_.getIneligibleObjs().begin(),
                      state_.getIneligibleObjs().end());

    // C7: dHat[b,v] = d[b,v] AND m[b,v]
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : allTracked) {
            BlockVarKey key = std::make_pair(block, V);
            GRBVar inputs[] = {d_[key], m_[key]};
            model_.addGenConstrAnd(dHat_[key], inputs, 2,
                                   "C7_dHat_and_" + nodeToken(cfg_, block) + "_" + valueToken(V));
        }
    }

    // C8 + LP tightening for s variables.
    for (const auto &[key, sVar] : s_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;
        std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);

        // C8': s[b,v] <= r[b]
        model_.addConstr(sVar <= r_[block], "C8p_save_le_r_" + suffix);

        // C8': s[b,v] <= Σ_pred dHat[pred,v]
        GRBLinExpr predDHatSum = 0;
        for (NodeId pred : predecessors_[block]) {
            predDHatSum += dHat_[std::make_pair(pred, V)];
        }
        model_.addConstr(sVar <= predDHatSum, "C8p_save_le_preds_" + suffix);
    }

    // C8: s[succ,v] >= dHat[pred,v] + r[succ] - 1  (∀ edge, v where s exists)
    // Build per-block save variable lookup for efficiency.
    std::map<NodeId, std::vector<std::pair<llvm::Value *, GRBVar>>> saveByBlock;
    for (const auto &[key, sVar] : s_) {
        saveByBlock[key.first].emplace_back(key.second, sVar);
    }

    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        auto it = saveByBlock.find(succ);
        if (it == saveByBlock.end())
            continue;
        for (const auto &[V, sVar] : it->second) {
            model_.addConstr(sVar >= dHat_[std::make_pair(pred, V)] + r_[succ] - 1,
                             "C8_save_edge_" + std::to_string(idx));
            idx++;
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
                                     "C9_energy_init_" + nodeToken(cfg_, block));
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
        model_.addConstr(eAccum_[src] + eBlkSrc + eEndDst <= Ebuf,
                         "C11_energy_edge_" + std::to_string(edgeIdx++));
    }

    for (NodeId exitBlock : cfg_.getExitBlocks()) {
        GRBLinExpr eBlkExit = buildEBlk(exitBlock);
        model_.addConstr(eAccum_[exitBlock] + eBlkExit <= Ebuf,
                         "C11_energy_exit_" + nodeToken(cfg_, exitBlock));
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

// E_start(b) = E_pro * r[b] + Σ_v E_rst_v * rHat[b,v]
GRBLinExpr CheckpointOptimizer::buildEStart(NodeId block) {
    GRBLinExpr expr = params_.E_pro * r_[block];
    for (const auto &[key, rHatVar] : rHat_) {
        if (key.first != block)
            continue;
        double eRestore = energy_.getERestore(key.second);
        if (eRestore > 0.0) {
            expr += eRestore * rHatVar;
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
