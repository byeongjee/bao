#include "milp/CheckpointOptimizer.h"

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

static std::string makeVarName(const char *prefix,
                               const std::string &block,
                               llvm::GlobalVariable *gv) {
    return std::string(prefix) + "_" + sanitizeToken(block) + "_" +
           sanitizeToken(gv->getName());
}

static std::map<std::string, std::vector<std::string>>
buildPredecessorMap(const CFGAnalysis &cfg) {
    std::map<std::string, std::vector<std::string>> preds;
    for (const auto &block : cfg.getBlocks()) {
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

std::vector<std::string> CheckpointOptimizer::getInfeasibleBlocks() const {
    std::vector<std::string> infeasible;
    for (const auto &blockName : cfg_.getBlocks()) {
        const BlockInfo &info = cfg_.getBlockInfo(blockName);
        if (info.energyCost > params_.capacity) {
            infeasible.push_back(blockName);
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
    const std::string &entry = cfg_.getEntryBlock();

    for (const auto &block : cfg_.getBlocks()) {
        isRegionStart_[block] = model_.addVar(
            0.0, 1.0, 0.0, GRB_BINARY,
            "is_region_start_" + sanitizeToken(block));
    }

    for (const auto &block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockGVKey key = std::make_pair(block, GV);
            placeInVm_[key] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY, makeVarName("place_in_vm", block, GV));
            pending_[key] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY, makeVarName("pending", block, GV));
            vmPending_[key] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY, makeVarName("vm_pending", block, GV));
        }
    }

    for (const auto &block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjLiveIn(block)) {
            BlockGVKey key = std::make_pair(block, GV);
            needRestore_[key] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY, makeVarName("need_restore", block, GV));
            if (block != entry) {
                commit_[key] = model_.addVar(
                    0.0, 1.0, 0.0, GRB_BINARY, makeVarName("commit", block, GV));
            }
        }
    }

    for (const auto &block : cfg_.getBlocks()) {
        energyAccumulated_[block] = model_.addVar(
            0.0, Ebuf, 0.0, GRB_CONTINUOUS,
            "energy_accumulated_" + sanitizeToken(block));
    }

    model_.update();
}

void CheckpointOptimizer::addObjective() {
    GRBLinExpr objective = 0;
    const std::string &entry = cfg_.getEntryBlock();

    // Term 1: placement penalty.
    for (const auto &block : cfg_.getBlocks()) {
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
    for (const auto &block : cfg_.getBlocks()) {
        objective += energy_.getFEntry(block) * buildEStart(block);
    }

    // Term 3: expected region-end overhead (excluding entry).
    for (const auto &block : cfg_.getBlocks()) {
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
    const std::string &entry = cfg_.getEntryBlock();
    if (!entry.empty() && isRegionStart_.count(entry)) {
        model_.addConstr(isRegionStart_[entry] == 1, "C1_entry_region_start");
    }
}

void CheckpointOptimizer::addC3_VMCapacity() {
    if (state_.getVMObjs().empty())
        return;

    for (const auto &block : cfg_.getBlocks()) {
        GRBLinExpr vmUsage = 0;
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            int elemId = state_.getVMObjStateElemId(GV);
            if (elemId < 0)
                continue;
            const StateElement &elem =
                state_.getStateElement(static_cast<unsigned>(elemId));
            vmUsage +=
                static_cast<double>(elem.sizeBytes) * placeInVm_[std::make_pair(block, GV)];
        }
        model_.addConstr(vmUsage <= static_cast<double>(params_.vmCapacityBytes),
                         "C3_vm_capacity_" + sanitizeToken(block));
    }
}

void CheckpointOptimizer::addC4_NeedVolLinearization() {
    for (const auto &[key, needVar] : needRestore_) {
        const std::string &block = key.first;
        BlockGVKey placeKey = std::make_pair(block, key.second);
        auto placeIt = placeInVm_.find(placeKey);
        if (placeIt == placeInVm_.end())
            continue;

        std::string suffix = sanitizeToken(block) + "_" + sanitizeToken(key.second->getName());
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
            model_.addConstr(
                pSucc <= pPred + xSucc,
                "C5a_place_prop_" + std::to_string(idx));
            model_.addConstr(
                pSucc >= pPred - xSucc,
                "C5b_place_prop_" + std::to_string(idx));
            idx++;
        }
    }
}

void CheckpointOptimizer::addC6_PendingPropagation() {
    auto preds = buildPredecessorMap(cfg_);

    // (1), (3), (4)
    for (const auto &block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockGVKey key = std::make_pair(block, GV);
            GRBVar p = pending_[key];
            double def = state_.getDefIndicator(block, GV) ? 1.0 : 0.0;

            std::string suffix = sanitizeToken(block) + "_" + sanitizeToken(GV->getName());
            model_.addConstr(p >= def, "C6a_pending_local_" + suffix);

            GRBLinExpr predSum = 0;
            for (const auto &pred : preds[block]) {
                predSum += pending_[std::make_pair(pred, GV)];
            }
            model_.addConstr(p <= def + predSum, "C6b_pending_ub_" + suffix);
            model_.addConstr(p <= def + (1 - isRegionStart_[block]),
                             "C6c_pending_reset_" + suffix);
        }
    }

    // (2)
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            model_.addConstr(
                pending_[std::make_pair(succ, GV)] >=
                    pending_[std::make_pair(pred, GV)] - isRegionStart_[succ],
                "C6d_pending_edge_" + std::to_string(idx));
            idx++;
        }
    }
}

void CheckpointOptimizer::addC7_CommitModel() {
    auto preds = buildPredecessorMap(cfg_);

    // vm_pending[b,v] = pending[b,v] AND place_in_vm[b,v]
    for (const auto &block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockGVKey key = std::make_pair(block, GV);
            GRBVar vmP = vmPending_[key];
            GRBVar p = pending_[key];
            GRBVar place = placeInVm_[key];
            std::string suffix = sanitizeToken(block) + "_" + sanitizeToken(GV->getName());
            model_.addConstr(vmP <= p, "C7a_vmp_le_pending_" + suffix);
            model_.addConstr(vmP <= place, "C7b_vmp_le_place_" + suffix);
            model_.addConstr(vmP >= p + place - 1, "C7c_vmp_ge_and_" + suffix);
        }
    }

    // commit[b,v] model (for b != entry and v in LiveIn(b))
    for (const auto &[key, commitVar] : commit_) {
        const std::string &block = key.first;
        llvm::GlobalVariable *GV = key.second;
        std::string suffix = sanitizeToken(block) + "_" + sanitizeToken(GV->getName());

        model_.addConstr(commitVar <= isRegionStart_[block], "C7d_commit_le_x_" + suffix);

        GRBLinExpr predVmPending = 0;
        for (const auto &pred : preds[block]) {
            predVmPending += vmPending_[std::make_pair(pred, GV)];
        }
        model_.addConstr(commitVar <= predVmPending, "C7e_commit_le_preds_" + suffix);

        for (const auto &pred : preds[block]) {
            model_.addConstr(
                commitVar >= isRegionStart_[block] +
                                 vmPending_[std::make_pair(pred, GV)] - 1,
                "C7f_commit_ge_pred_" + sanitizeToken(pred) + "_" + suffix);
        }
    }
}

void CheckpointOptimizer::addC8_EnergyInit() {
    const double M = params_.capacity;
    for (const auto &block : cfg_.getBlocks()) {
        GRBLinExpr eStart = buildEStart(block);
        model_.addConstr(
            energyAccumulated_[block] >=
                eStart - M * (1 - isRegionStart_[block]),
            "C8a_einit_lb_" + sanitizeToken(block));
        model_.addConstr(
            energyAccumulated_[block] <=
                eStart + M * (1 - isRegionStart_[block]),
            "C8b_einit_ub_" + sanitizeToken(block));
    }
}

void CheckpointOptimizer::addC9_EnergyPropagation() {
    const double M = params_.capacity;
    unsigned edgeIdx = 0;
    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        model_.addConstr(
            energyAccumulated_[dst] >=
                energyAccumulated_[src] + eBlkSrc - M * isRegionStart_[dst],
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

    for (const auto &exitBlock : cfg_.getExitBlocks()) {
        GRBLinExpr eBlkExit = buildEBlk(exitBlock);
        model_.addConstr(
            energyAccumulated_[exitBlock] + eBlkExit <= Ebuf,
            "C10_exit_" + sanitizeToken(exitBlock));
    }
}

GRBLinExpr CheckpointOptimizer::buildEBlk(const std::string &block) {
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

GRBLinExpr CheckpointOptimizer::buildEStart(const std::string &block) {
    GRBLinExpr expr = 0;
    expr += params_.E_pro * isRegionStart_[block];

    double qb = energy_.getQReboot(block);
    for (llvm::GlobalVariable *GV : state_.getVMObjLiveIn(block)) {
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

GRBLinExpr CheckpointOptimizer::buildEEnd(const std::string &block) {
    GRBLinExpr expr = 0;
    expr += params_.E_epi * isRegionStart_[block];

    for (llvm::GlobalVariable *GV : state_.getVMObjLiveIn(block)) {
        auto it = commit_.find(std::make_pair(block, GV));
        if (it == commit_.end())
            continue;
        double eSave = energy_.getESave(GV);
        if (eSave > 0.0) {
            expr += eSave * it->second;
        }
    }

    return expr;
}

void CheckpointOptimizer::extractSolution() {
    for (const auto &[blockName, var] : isRegionStart_) {
        if (var.get(GRB_DoubleAttr_X) > 0.5) {
            solution_.regionStarts.insert(blockName);
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

    for (const auto &[blockName, var] : energyAccumulated_) {
        solution_.energyAccumulated[blockName] = var.get(GRB_DoubleAttr_X);
    }

    solution_.objectiveValue = model_.get(GRB_DoubleAttr_ObjVal);
}

} // namespace checkpoint
