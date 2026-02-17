#include "milp/CheckpointOptimizer.h"

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

static std::string makeVarName(const ICFGView &cfg,
                               const char *prefix,
                               NodeId block,
                               llvm::Value *v) {
    std::string vName;
    if (v->hasName()) {
        vName = sanitizeToken(v->getName());
    } else {
        // Unnamed values: use a stable string representation.
        std::string raw;
        llvm::raw_string_ostream rso(raw);
        v->printAsOperand(rso, false);
        vName = sanitizeToken(rso.str());
    }
    return std::string(prefix) + "_" + nodeToken(cfg, block) + "_" + vName;
}

static std::string makeVarNameGV(const ICFGView &cfg,
                                 const char *prefix,
                                 NodeId block,
                                 llvm::GlobalVariable *gv) {
    return std::string(prefix) + "_" + nodeToken(cfg, block) + "_" +
           sanitizeToken(gv->getName());
}

static std::map<NodeId, std::vector<NodeId>> buildPredecessorMap(
    const ICFGView &cfg) {
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
    : cfg_(input.cfg),
      state_(input.state),
      energy_(input.energy),
      params_(input.energy.getParams()),
      env_(),
      model_(env_) {
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
        llvm::errs() << "Error: Blocks exceed capacity\n";
        return false;
    }

    buildModel();
    model_.optimize();
    solved_ = true;

    int status = model_.get(GRB_IntAttr_Status);
    if (status == GRB_OPTIMAL) {
        extractSolution();
        solution_.solverStatus = SolverStatus::Optimal;
        return true;
    }

    int solCount = model_.get(GRB_IntAttr_SolCount);
    llvm::errs() << "Optimization did not prove optimality"
                 << " (Gurobi status=" << status
                 << ", solutions found=" << solCount << ")\n";

    if (status == GRB_INFEASIBLE) {
        LLVM_DEBUG({
            llvm::dbgs() << "Computing IIS...\n";
            model_.computeIIS();
            auto constrs = model_.getConstrs();
            for (int i = 0; i < model_.get(GRB_IntAttr_NumConstrs); i++) {
                if (constrs[i].get(GRB_IntAttr_IISConstr)) {
                    llvm::dbgs() << "  IIS constr: "
                                 << constrs[i].get(GRB_StringAttr_ConstrName)
                                 << "\n";
                }
            }
            auto vars = model_.getVars();
            for (int i = 0; i < model_.get(GRB_IntAttr_NumVars); i++) {
                if (vars[i].get(GRB_IntAttr_IISLB) ||
                    vars[i].get(GRB_IntAttr_IISUB)) {
                    llvm::dbgs() << "  IIS var bound: "
                                 << vars[i].get(GRB_StringAttr_VarName)
                                 << " LB=" << vars[i].get(GRB_IntAttr_IISLB)
                                 << " UB=" << vars[i].get(GRB_IntAttr_IISUB)
                                 << "\n";
                }
            }
        });
    }

    if (acceptFeasible_ && solCount > 0) {
        double gap = model_.get(GRB_DoubleAttr_MIPGap);
        llvm::errs() << "Accepting feasible solution (MIP gap=" << gap << ")\n";
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
    addConstraints();
    model_.update();
}

void CheckpointOptimizer::addVariables() {
    const double Ebuf = params_.capacity;
    NodeId entry = cfg_.getEntryBlock();

    for (NodeId block : cfg_.getBlocks()) {
        isRegionStart_[block] = model_.addVar(
            0.0, 1.0, 0.0, GRB_BINARY,
            "is_region_start_" + nodeToken(cfg_, block));
    }

    // Eligible (candidate) variables: placeInVm, pending, vmPending.
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockGVKey gvKey = std::make_pair(block, GV);
            BlockVarKey varKey = std::make_pair(block, static_cast<llvm::Value *>(GV));
            placeInVm_[gvKey] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                makeVarNameGV(cfg_, "place_in_vm", block, GV));
            pending_[varKey] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                makeVarNameGV(cfg_, "pending", block, GV));
            vmPending_[gvKey] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                makeVarNameGV(cfg_, "vm_pending", block, GV));
        }
    }

    // Ineligible variables: pending only (no placeInVm, no vmPending).
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : state_.getIneligibleObjs()) {
            BlockVarKey varKey = std::make_pair(block, V);
            pending_[varKey] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                makeVarName(cfg_, "pending", block, V));
        }
    }

    // needRestore (eligible only, for v in EligLiveIn(b)).
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getEligLiveIn(block)) {
            BlockGVKey gvKey = std::make_pair(block, GV);
            needRestore_[gvKey] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                makeVarNameGV(cfg_, "need_restore", block, GV));
        }
    }

    // commit: eligible live-ins + ineligible live-ins (for b != entry).
    for (NodeId block : cfg_.getBlocks()) {
        if (block == entry)
            continue;
        // Eligible commits.
        for (llvm::GlobalVariable *GV : state_.getEligLiveIn(block)) {
            BlockVarKey varKey = std::make_pair(block, static_cast<llvm::Value *>(GV));
            commit_[varKey] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                makeVarNameGV(cfg_, "commit", block, GV));
        }
        // Ineligible commits.
        for (llvm::Value *V : state_.getIneligLiveIn(block)) {
            BlockVarKey varKey = std::make_pair(block, V);
            commit_[varKey] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                makeVarName(cfg_, "commit", block, V));
        }
    }

    for (NodeId block : cfg_.getBlocks()) {
        energyAccumulated_[block] = model_.addVar(
            0.0, Ebuf, 0.0, GRB_CONTINUOUS,
            "energy_accumulated_" + nodeToken(cfg_, block));
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
            objective -= fEntry * eNvm * placeInVm_[std::make_pair(block, GV)];
        }
    }

    // Term 2: expected region-start overhead.
    for (NodeId block : cfg_.getBlocks()) {
        objective += energy_.getFEntry(block) * buildEStart(block);
    }

    // Term 3: expected region-end overhead (excluding entry).
    for (NodeId block : cfg_.getBlocks()) {
        if (block == entry)
            continue;
        objective += energy_.getFEntry(block) * buildEEnd(block);
    }

    model_.setObjective(objective, GRB_MINIMIZE);
}

void CheckpointOptimizer::addConstraints() {
    addC1_EntryRegionStart();
    addC3_VMCapacity();
    addC4_NeedVolLinearization();
    addC5_PlacementPropagation();
    addC6_PendingPropagation();
    addC7_CommitModel();
    addC8_EnergyInit();
    addC9_EnergyPropagation();
    addC10_BufferSafety();
}

void CheckpointOptimizer::addC1_EntryRegionStart() {
    NodeId entry = cfg_.getEntryBlock();
    if (entry != kInvalidNodeId && isRegionStart_.count(entry)) {
        model_.addConstr(isRegionStart_[entry] == 1, "C1_entry_region_start");
    }
}

void CheckpointOptimizer::addC3_VMCapacity() {
    if (state_.getVMObjs().empty() && state_.getIneligibleObjs().empty())
        return;

    // Memory-backed ineligible objects always occupy VM (e.g., globals/allocas).
    // Cross-block SSA values are checkpoint-tracked state but do not consume
    // persistent VM capacity.
    double ineligibleSize = 0;
    for (llvm::Value *V : state_.getIneligibleObjs()) {
        if (!llvm::isa<llvm::GlobalVariable>(V) &&
            !llvm::isa<llvm::AllocaInst>(V)) {
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
                       placeInVm_[std::make_pair(block, GV)];
        }
        model_.addConstr(
            vmUsage + ineligibleSize <=
                static_cast<double>(params_.vmCapacityBytes),
            "C3_vm_capacity_" + nodeToken(cfg_, block));
    }
}

void CheckpointOptimizer::addC4_NeedVolLinearization() {
    for (const auto &[key, needVar] : needRestore_) {
        NodeId block = key.first;
        BlockGVKey placeKey = std::make_pair(block, key.second);
        auto placeIt = placeInVm_.find(placeKey);
        if (placeIt == placeInVm_.end())
            continue;

        std::string suffix =
            nodeToken(cfg_, block) + "_" + sanitizeToken(key.second->getName());
        model_.addConstr(needVar <= isRegionStart_[block], "C4a_need_le_x_" + suffix);
        model_.addConstr(needVar <= placeIt->second, "C4b_need_le_p_" + suffix);
        model_.addConstr(needVar >= isRegionStart_[block] + placeIt->second - 1,
                         "C4c_need_ge_xp_" + suffix);
    }
}

void CheckpointOptimizer::addC5_PlacementPropagation() {
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            GRBVar pPred = placeInVm_[std::make_pair(pred, GV)];
            GRBVar pSucc = placeInVm_[std::make_pair(succ, GV)];
            GRBVar xSucc = isRegionStart_[succ];
            model_.addConstr(pSucc <= pPred + xSucc,
                             "C5a_place_prop_" + std::to_string(idx));
            model_.addConstr(pSucc >= pPred - xSucc,
                             "C5b_place_prop_" + std::to_string(idx));
            idx++;
        }
    }
}

void CheckpointOptimizer::addC6_PendingPropagation() {
    auto preds = buildPredecessorMap(cfg_);

    // Build combined list of all tracked variables as Value* (elig + inelig).
    std::vector<llvm::Value *> allTracked;
    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        allTracked.push_back(static_cast<llvm::Value *>(GV));
    allTracked.insert(allTracked.end(),
                      state_.getIneligibleObjs().begin(),
                      state_.getIneligibleObjs().end());

    // (1), (3), (4)
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::Value *V : allTracked) {
            BlockVarKey key = std::make_pair(block, V);
            GRBVar p = pending_[key];

            // Def indicator: check eligible then ineligible.
            bool isDef = false;
            if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
                isDef = state_.getEligDefIndicator(block, GV);
            if (!isDef)
                isDef = state_.getIneligDefIndicator(block, V);
            double def = isDef ? 1.0 : 0.0;

            std::string vName;
            if (V->hasName()) {
                vName = sanitizeToken(V->getName());
            } else {
                std::string raw;
                llvm::raw_string_ostream rso(raw);
                V->printAsOperand(rso, false);
                vName = sanitizeToken(rso.str());
            }
            std::string suffix = nodeToken(cfg_, block) + "_" + vName;
            model_.addConstr(p >= def, "C6a_pending_local_" + suffix);

            GRBLinExpr predSum = 0;
            for (NodeId pred : preds[block]) {
                predSum += pending_[std::make_pair(pred, V)];
            }
            model_.addConstr(p <= def + predSum, "C6b_pending_ub_" + suffix);
            model_.addConstr(p <= def + (1 - isRegionStart_[block]),
                             "C6c_pending_reset_" + suffix);
        }
    }

    // (2)
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::Value *V : allTracked) {
            model_.addConstr(
                pending_[std::make_pair(succ, V)] >=
                    pending_[std::make_pair(pred, V)] - isRegionStart_[succ],
                "C6d_pending_edge_" + std::to_string(idx));
            idx++;
        }
    }
}

void CheckpointOptimizer::addC7_CommitModel() {
    auto preds = buildPredecessorMap(cfg_);

    // C7.0: vm_pending[b,v] = pending[b,v] AND place_in_vm[b,v] — eligibles only
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockGVKey gvKey = std::make_pair(block, GV);
            BlockVarKey varKey = std::make_pair(block, static_cast<llvm::Value *>(GV));
            GRBVar vmP = vmPending_[gvKey];
            GRBVar p = pending_[varKey];
            GRBVar place = placeInVm_[gvKey];
            std::string suffix =
                nodeToken(cfg_, block) + "_" + sanitizeToken(GV->getName());
            model_.addConstr(vmP <= p, "C7a_vmp_le_pending_" + suffix);
            model_.addConstr(vmP <= place, "C7b_vmp_le_place_" + suffix);
            model_.addConstr(vmP >= p + place - 1, "C7c_vmp_ge_and_" + suffix);
        }
    }

    // commit[b,v] model (for b != entry and v in LiveIn(b))
    for (const auto &[key, commitVar] : commit_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;

        std::string vName;
        if (V->hasName()) {
            vName = sanitizeToken(V->getName());
        } else {
            std::string raw;
            llvm::raw_string_ostream rso(raw);
            V->printAsOperand(rso, false);
            vName = sanitizeToken(rso.str());
        }
        std::string suffix = nodeToken(cfg_, block) + "_" + vName;

        model_.addConstr(commitVar <= isRegionStart_[block],
                         "C7d_commit_le_x_" + suffix);

        if (state_.isIneligible(V)) {
            // C7.1: Ineligible commit — uses pending directly (always in VM).
            GRBLinExpr predPending = 0;
            for (NodeId pred : preds[block]) {
                predPending += pending_[std::make_pair(pred, V)];
            }
            model_.addConstr(commitVar <= predPending,
                             "C7e_inelig_commit_le_preds_" + suffix);
            for (NodeId pred : preds[block]) {
                model_.addConstr(
                    commitVar >= isRegionStart_[block] +
                                     pending_[std::make_pair(pred, V)] - 1,
                    "C7f_inelig_commit_ge_pred_" + nodeToken(cfg_, pred) +
                        "_" + suffix);
            }
        } else {
            // C7.2: Eligible commit — uses vmPending.
            auto *GV = llvm::cast<llvm::GlobalVariable>(V);
            GRBLinExpr predVmPending = 0;
            for (NodeId pred : preds[block]) {
                predVmPending += vmPending_[std::make_pair(pred, GV)];
            }
            model_.addConstr(commitVar <= predVmPending,
                             "C7e_commit_le_preds_" + suffix);
            for (NodeId pred : preds[block]) {
                model_.addConstr(
                    commitVar >= isRegionStart_[block] +
                                     vmPending_[std::make_pair(pred, GV)] - 1,
                    "C7f_commit_ge_pred_" + nodeToken(cfg_, pred) + "_" +
                        suffix);
            }
        }
    }
}

void CheckpointOptimizer::addC8_EnergyInit() {
    const double M = params_.capacity;
    for (NodeId block : cfg_.getBlocks()) {
        GRBLinExpr eStart = buildEStart(block);
        model_.addConstr(energyAccumulated_[block] >=
                             eStart - M * (1 - isRegionStart_[block]),
                         "C8a_einit_lb_" + nodeToken(cfg_, block));
        model_.addConstr(energyAccumulated_[block] <=
                             eStart + M * (1 - isRegionStart_[block]),
                         "C8b_einit_ub_" + nodeToken(cfg_, block));
    }
}

void CheckpointOptimizer::addC9_EnergyPropagation() {
    const double M = params_.capacity;
    unsigned edgeIdx = 0;
    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        model_.addConstr(energyAccumulated_[dst] >=
                             energyAccumulated_[src] + eBlkSrc -
                                 M * isRegionStart_[dst],
                         "C9_propagate_" + std::to_string(edgeIdx++));
    }
}

void CheckpointOptimizer::addC10_BufferSafety() {
    const double Ebuf = params_.capacity;
    unsigned edgeIdx = 0;

    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        GRBLinExpr eEndDst = buildEEnd(dst);
        model_.addConstr(
            energyAccumulated_[src] + eBlkSrc + eEndDst <= Ebuf,
            "C10_edge_" + std::to_string(edgeIdx++));
    }

    for (NodeId exitBlock : cfg_.getExitBlocks()) {
        GRBLinExpr eBlkExit = buildEBlk(exitBlock);
        model_.addConstr(energyAccumulated_[exitBlock] + eBlkExit <= Ebuf,
                         "C10_exit_" + nodeToken(cfg_, exitBlock));
    }
}

GRBLinExpr CheckpointOptimizer::buildEBlk(NodeId block) {
    GRBLinExpr expr = energy_.getEBase(block);
    for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
        double eNvm = energy_.getENvm(block, GV);
        if (eNvm == 0.0)
            continue;
        expr += eNvm;
        expr -= eNvm * placeInVm_[std::make_pair(block, GV)];
    }
    return expr;
}

GRBLinExpr CheckpointOptimizer::buildEStart(NodeId block) {
    GRBLinExpr expr = 0;
    double qb = energy_.getQReboot();

    // Ineligible restore cost: unconditional at region start (constant coeff).
    double ineligRestoreCost = 0;
    for (llvm::Value *V : state_.getIneligLiveIn(block)) {
        double eRestore = energy_.getERestore(V);
        if (eRestore > 0.0)
            ineligRestoreCost += eRestore;
    }
    expr += (params_.E_pro + qb * ineligRestoreCost) * isRegionStart_[block];

    // Eligible restore cost: variable (depends on needRestore).
    for (llvm::GlobalVariable *GV : state_.getEligLiveIn(block)) {
        auto it = needRestore_.find(std::make_pair(block, GV));
        if (it == needRestore_.end())
            continue;
        double eRestore = energy_.getERestore(GV);
        if (eRestore > 0.0) {
            expr += qb * eRestore * it->second;
        }
    }

    return expr;
}

GRBLinExpr CheckpointOptimizer::buildEEnd(NodeId block) {
    GRBLinExpr expr = 0;
    expr += params_.E_epi * isRegionStart_[block];

    for (const auto &[key, commitVar] : commit_) {
        if (key.first != block)
            continue;
        double eSave = energy_.getESave(key.second);
        if (eSave > 0.0) {
            expr += eSave * commitVar;
        }
    }

    return expr;
}

void CheckpointOptimizer::extractSolution() {
    for (const auto &[block, var] : isRegionStart_) {
        if (var.get(GRB_DoubleAttr_X) > 0.5) {
            solution_.regionStarts.insert(block);
        }
    }

    for (const auto &[key, var] : placeInVm_) {
        solution_.placeInVm[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    for (const auto &[key, var] : needRestore_) {
        solution_.needRestore[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    for (const auto &[key, var] : commit_) {
        solution_.commit[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    for (const auto &[block, var] : energyAccumulated_) {
        solution_.energyAccumulated[block] = var.get(GRB_DoubleAttr_X);
    }

    solution_.objectiveValue = model_.get(GRB_DoubleAttr_ObjVal);
}

} // namespace checkpoint
