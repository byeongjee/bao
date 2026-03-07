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

/// Return a sanitized token for any Value (named or unnamed).
static std::string valueToken(llvm::Value *V) {
    if (V->hasName())
        return sanitizeToken(V->getName());
    std::string raw;
    llvm::raw_string_ostream rso(raw);
    V->printAsOperand(rso, false);
    return sanitizeToken(rso.str());
}

static std::string makeVarName(const ICFGView &cfg,
                               const char *prefix,
                               NodeId block,
                               llvm::Value *v) {
    return std::string(prefix) + "_" + nodeToken(cfg, block) + "_" +
           valueToken(v);
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

/// Linearize z = a AND b for binary variables: z <= a, z <= b, z >= a+b-1.
static void addAndLinearization(GRBModel &model,
                                GRBVar z, GRBVar a, GRBVar b,
                                const std::string &namePrefix) {
    model.addConstr(z <= a, namePrefix + "_le_a");
    model.addConstr(z <= b, namePrefix + "_le_b");
    model.addConstr(z >= a + b - 1, namePrefix + "_ge_ab");
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
    llvm::errs() << "Optimization did not prove optimality"
                 << " (Gurobi status=" << status
                 << ", solutions found=" << solCount << ")\n";

    if (status == GRB_INFEASIBLE) {
        llvm::errs() << "Optimization infeasible; computing IIS diagnostics...\n";
        model_.computeIIS();
        model_.write("milp_infeasible.lp");
        model_.write("milp_infeasible.ilp");
        llvm::errs() << "  Wrote model: milp_infeasible.lp\n";
        llvm::errs() << "  Wrote IIS:   milp_infeasible.ilp\n";

        auto constrs = model_.getConstrs();
        int numConstrs = model_.get(GRB_IntAttr_NumConstrs);
        int printedConstrs = 0;
        for (int i = 0; i < numConstrs; i++) {
            if (!constrs[i].get(GRB_IntAttr_IISConstr))
                continue;
            llvm::errs() << "  IIS constr: "
                         << constrs[i].get(GRB_StringAttr_ConstrName) << "\n";
            printedConstrs++;
            if (printedConstrs >= 200) {
                llvm::errs() << "  IIS constr: ... truncated at 200 entries\n";
                break;
            }
        }

        auto vars = model_.getVars();
        int numVars = model_.get(GRB_IntAttr_NumVars);
        int printedVarBounds = 0;
        for (int i = 0; i < numVars; i++) {
            if (!vars[i].get(GRB_IntAttr_IISLB) &&
                !vars[i].get(GRB_IntAttr_IISUB))
                continue;
            llvm::errs() << "  IIS var bound: "
                         << vars[i].get(GRB_StringAttr_VarName)
                         << " LB=" << vars[i].get(GRB_IntAttr_IISLB)
                         << " UB=" << vars[i].get(GRB_IntAttr_IISUB) << "\n";
            printedVarBounds++;
            if (printedVarBounds >= 200) {
                llvm::errs()
                    << "  IIS var bound: ... truncated at 200 entries\n";
                break;
            }
        }
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
    predecessors_ = buildPredecessorMap(cfg_);
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
    constrainEntryAsRegionStart();
    constrainVMCapacity();
    constrainNeedRestoreLinearization();
    constrainPlacementPropagation();
    constrainPendingStatePropagation();
    constrainCommitAtRegionBoundary();
    constrainEnergyInitAtRegionStart();
    constrainEnergyPropagation();
    constrainEnergyWithinCapacity();
}

// Entry block is always a region start:
//   isRegionStart[entry] = 1
void CheckpointOptimizer::constrainEntryAsRegionStart() {
    NodeId entry = cfg_.getEntryBlock();
    if (entry != kInvalidNodeId && isRegionStart_.count(entry)) {
        model_.addConstr(isRegionStart_[entry] == 1, "entry_region_start");
    }
}

// Per-block VM (SRAM) capacity limit:
//   sum_v( size(v) * placeInVm[b,v] ) + ineligibleSize <= VM_capacity
// Eligible objects may or may not be in VM; ineligible memory objects always are.
void CheckpointOptimizer::constrainVMCapacity() {
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
            "vm_capacity_" + nodeToken(cfg_, block));
    }
}

// needRestore[b,v] = isRegionStart[b] AND placeInVm[b,v]
// A variable needs restoring from FRAM only when a new region begins
// and the variable is placed in volatile memory (VM/SRAM).
void CheckpointOptimizer::constrainNeedRestoreLinearization() {
    for (const auto &[key, needVar] : needRestore_) {
        NodeId block = key.first;
        BlockGVKey placeKey = std::make_pair(block, key.second);
        auto placeIt = placeInVm_.find(placeKey);
        if (placeIt == placeInVm_.end())
            continue;

        std::string prefix =
            "need_restore_" + nodeToken(cfg_, block) + "_" +
            sanitizeToken(key.second->getName());
        addAndLinearization(model_, needVar, isRegionStart_[block],
                            placeIt->second, prefix);
    }
}

// Placement consistency across CFG edges:
//   placeInVm[succ,v] <= placeInVm[pred,v] + isRegionStart[succ]
//   placeInVm[succ,v] >= placeInVm[pred,v] - isRegionStart[succ]
// Within a region (isRegionStart=0), placement is inherited from predecessors.
// At region boundaries, placement may change freely.
void CheckpointOptimizer::constrainPlacementPropagation() {
    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            GRBVar pPred = placeInVm_[std::make_pair(pred, GV)];
            GRBVar pSucc = placeInVm_[std::make_pair(succ, GV)];
            GRBVar xSucc = isRegionStart_[succ];
            model_.addConstr(pSucc <= pPred + xSucc,
                             "placement_prop_fwd_" + std::to_string(idx));
            model_.addConstr(pSucc >= pPred - xSucc,
                             "placement_prop_bwd_" + std::to_string(idx));
            idx++;
        }
    }
}

// Pending-state propagation for all tracked variables (eligible + ineligible):
//   pending[b,v] >= def(b,v)                                       (local def)
//   pending[b,v] <= def(b,v) + sum_{p in preds(b)} pending[p,v]    (upper bound)
//   pending[b,v] <= def(b,v) + (1 - isRegionStart[b])              (region reset)
//   pending[succ,v] >= pending[pred,v] - isRegionStart[succ]       (edge prop)
// Tracks whether variable v has an uncommitted modification reaching block b.
// Region starts reset pending state (forcing a commit at the boundary).
void CheckpointOptimizer::constrainPendingStatePropagation() {
    // Build combined list of all tracked variables as Value* (elig + inelig).
    std::vector<llvm::Value *> allTracked;
    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        allTracked.push_back(static_cast<llvm::Value *>(GV));
    allTracked.insert(allTracked.end(),
                      state_.getIneligibleObjs().begin(),
                      state_.getIneligibleObjs().end());

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

            std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);
            model_.addConstr(p >= def, "pending_local_def_" + suffix);

            GRBLinExpr predSum = 0;
            for (NodeId pred : predecessors_[block]) {
                predSum += pending_[std::make_pair(pred, V)];
            }
            model_.addConstr(p <= def + predSum,
                             "pending_upper_bound_" + suffix);
            model_.addConstr(p <= def + (1 - isRegionStart_[block]),
                             "pending_region_reset_" + suffix);
        }
    }

    unsigned idx = 0;
    for (const auto &[pred, succ] : cfg_.getEdges()) {
        for (llvm::Value *V : allTracked) {
            model_.addConstr(
                pending_[std::make_pair(succ, V)] >=
                    pending_[std::make_pair(pred, V)] - isRegionStart_[succ],
                "pending_edge_prop_" + std::to_string(idx));
            idx++;
        }
    }
}

// Commit model — forces dirty state to be saved at region boundaries.
//
// vmPending[b,v] = pending[b,v] AND placeInVm[b,v]  (eligible only)
//   Tracks pending modifications for VM-placed eligible variables.
//
// For each commit variable (b != entry, v in LiveIn(b)):
//   commit[b,v] <= isRegionStart[b]                  (only at boundaries)
//   commit[b,v] <= sum_{p} state[p,v]                (some pred must be dirty)
//   commit[b,v] >= isRegionStart[b] + state[p,v] - 1 (for each pred p)
// where state = pending (ineligible) or vmPending (eligible).
void CheckpointOptimizer::constrainCommitAtRegionBoundary() {
    // vmPending[b,v] = pending[b,v] AND placeInVm[b,v] — eligibles only
    for (NodeId block : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            BlockGVKey gvKey = std::make_pair(block, GV);
            BlockVarKey varKey = std::make_pair(block, static_cast<llvm::Value *>(GV));
            std::string prefix =
                "vm_pending_and_" + nodeToken(cfg_, block) + "_" +
                sanitizeToken(GV->getName());
            addAndLinearization(model_, vmPending_[gvKey], pending_[varKey],
                                placeInVm_[gvKey], prefix);
        }
    }

    // commit[b,v] model (for b != entry and v in LiveIn(b))
    for (const auto &[key, commitVar] : commit_) {
        NodeId block = key.first;
        llvm::Value *V = key.second;

        std::string suffix = nodeToken(cfg_, block) + "_" + valueToken(V);

        model_.addConstr(commitVar <= isRegionStart_[block],
                         "commit_le_region_start_" + suffix);

        if (state_.isIneligible(V)) {
            // Ineligible commit — uses pending directly (always in VM).
            GRBLinExpr predPending = 0;
            for (NodeId pred : predecessors_[block]) {
                predPending += pending_[std::make_pair(pred, V)];
            }
            model_.addConstr(commitVar <= predPending,
                             "inelig_commit_le_preds_" + suffix);
            for (NodeId pred : predecessors_[block]) {
                model_.addConstr(
                    commitVar >= isRegionStart_[block] +
                                     pending_[std::make_pair(pred, V)] - 1,
                    "inelig_commit_ge_pred_" + nodeToken(cfg_, pred) +
                        "_" + suffix);
            }
        } else {
            // Eligible commit — uses vmPending.
            auto *GV = llvm::cast<llvm::GlobalVariable>(V);
            GRBLinExpr predVmPending = 0;
            for (NodeId pred : predecessors_[block]) {
                predVmPending += vmPending_[std::make_pair(pred, GV)];
            }
            model_.addConstr(commitVar <= predVmPending,
                             "elig_commit_le_preds_" + suffix);
            for (NodeId pred : predecessors_[block]) {
                model_.addConstr(
                    commitVar >= isRegionStart_[block] +
                                     vmPending_[std::make_pair(pred, GV)] - 1,
                    "elig_commit_ge_pred_" + nodeToken(cfg_, pred) + "_" +
                        suffix);
            }
        }
    }
}

// At region starts, accumulated energy equals the startup cost:
//   eAccum[b] >= E_start(b) - M*(1 - isRegionStart[b])
//   eAccum[b] <= E_start(b) + M*(1 - isRegionStart[b])
// Big-M deactivates the constraint for non-region-start blocks.
void CheckpointOptimizer::constrainEnergyInitAtRegionStart() {
    const double M = params_.capacity;
    for (NodeId block : cfg_.getBlocks()) {
        GRBLinExpr eStart = buildEStart(block);
        model_.addConstr(energyAccumulated_[block] >=
                             eStart - M * (1 - isRegionStart_[block]),
                         "energy_init_lb_" + nodeToken(cfg_, block));
        model_.addConstr(energyAccumulated_[block] <=
                             eStart + M * (1 - isRegionStart_[block]),
                         "energy_init_ub_" + nodeToken(cfg_, block));
    }
}

// Energy accumulates along CFG edges within a region:
//   eAccum[dst] >= eAccum[src] + E_blk(src) - M*isRegionStart[dst]
// Region starts break propagation (big-M deactivation).
void CheckpointOptimizer::constrainEnergyPropagation() {
    const double M = params_.capacity;
    unsigned edgeIdx = 0;
    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        model_.addConstr(energyAccumulated_[dst] >=
                             energyAccumulated_[src] + eBlkSrc -
                                 M * isRegionStart_[dst],
                         "energy_propagation_" + std::to_string(edgeIdx++));
    }
}

// Energy must not exceed buffer capacity on any path:
//   eAccum[src] + E_blk(src) + E_end(dst) <= capacity   (per edge)
//   eAccum[exit] + E_blk(exit) <= capacity               (at exits)
// Ensures every region can complete before the energy buffer is exhausted.
void CheckpointOptimizer::constrainEnergyWithinCapacity() {
    const double Ebuf = params_.capacity;
    unsigned edgeIdx = 0;

    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);
        GRBLinExpr eEndDst = buildEEnd(dst);
        model_.addConstr(
            energyAccumulated_[src] + eBlkSrc + eEndDst <= Ebuf,
            "energy_capacity_edge_" + std::to_string(edgeIdx++));
    }

    for (NodeId exitBlock : cfg_.getExitBlocks()) {
        GRBLinExpr eBlkExit = buildEBlk(exitBlock);
        model_.addConstr(energyAccumulated_[exitBlock] + eBlkExit <= Ebuf,
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
