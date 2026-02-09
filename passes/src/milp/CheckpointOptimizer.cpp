#include "milp/CheckpointOptimizer.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace checkpoint {

CheckpointOptimizer::CheckpointOptimizer(const MILPInput &input)
    : input_(input),
      cfg_(input.cfg),
      state_(input.state),
      energy_(input.energy),
      params_(input.energy.getParams()),
      env_(),
      model_(env_) {
    model_.set(GRB_IntParam_OutputFlag, 0);
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

    if (model_.get(GRB_IntAttr_Status) != GRB_OPTIMAL) {
        llvm::errs() << "Optimization failed: no optimal solution found\n";
        return false;
    }

    extractSolution();
    return true;
}

void CheckpointOptimizer::buildModel() {
    addVariables();
    addObjective();
    addConstraints();
    model_.update();
}

void CheckpointOptimizer::addVariables() {
    double Ebuf = params_.capacity;

    // is_region_start[b]: binary for each block
    for (const auto &blockName : cfg_.getBlocks()) {
        isRegionStart_[blockName] = model_.addVar(
            0.0, 1.0, 0.0, GRB_BINARY, "is_region_start_" + blockName);
    }

    // store_enabled[d]: binary for each def site
    for (const auto &ds : state_.getDefSites()) {
        storeEnabled_[ds.id] = model_.addVar(
            0.0, 1.0, 0.0, GRB_BINARY,
            "store_enabled_" + std::to_string(ds.id));
    }

    // placed_in_vm[v]: binary for each VMObj
    for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
        placedInVm_[GV] = model_.addVar(
            0.0, 1.0, 0.0, GRB_BINARY,
            "placed_in_vm_" + GV->getName().str());
    }

    // needs_vol_restore[b,v]: binary for each (block, VMObj) where VMObj is
    // live-in
    for (const auto &blockName : cfg_.getBlocks()) {
        const auto &liveVMObjs = state_.getVMObjLiveIn(blockName);
        for (llvm::GlobalVariable *GV : liveVMObjs) {
            int elemId = state_.getVMObjStateElemId(GV);
            if (elemId < 0)
                continue;
            auto key = std::make_pair(blockName, static_cast<unsigned>(elemId));
            needsVolRestore_[key] = model_.addVar(
                0.0, 1.0, 0.0, GRB_BINARY,
                "needs_vol_restore_" + blockName + "_" +
                    GV->getName().str());
        }
    }

    // energy_accumulated[b]: continuous [0, E_buf]
    for (const auto &blockName : cfg_.getBlocks()) {
        energyAccumulated_[blockName] = model_.addVar(
            0.0, Ebuf, 0.0, GRB_CONTINUOUS,
            "energy_accumulated_" + blockName);
    }

    model_.update();
}

void CheckpointOptimizer::addObjective() {
    GRBLinExpr objective = 0;

    // Term 1: NVM placement penalties
    // Sigma_b F_entry[b] * Sigma_v E_nvm[b,v] * (1 - placed_in_vm[v])
    for (const auto &blockName : cfg_.getBlocks()) {
        double fEntry = energy_.getFEntry(blockName);
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            double eNvm = energy_.getENvm(blockName, GV);
            if (eNvm == 0.0)
                continue;
            // eNvm * (1 - p_v) = eNvm - eNvm * p_v
            objective += fEntry * eNvm;
            objective -= fEntry * eNvm * placedInVm_[GV];
        }
    }

    // Term 2: Prologue overhead
    // Sigma_b F_entry[b] * is_region_start[b] * E_pro
    if (params_.E_pro > 0.0) {
        for (const auto &blockName : cfg_.getBlocks()) {
            double fEntry = energy_.getFEntry(blockName);
            objective += fEntry * params_.E_pro * isRegionStart_[blockName];
        }
    }

    // Term 3: Epilogue overhead (for all blocks except entry)
    // Sigma_{b != b0} F_entry[b] * is_region_start[b] * E_epi
    if (params_.E_epi > 0.0) {
        const std::string &entry = cfg_.getEntryBlock();
        for (const auto &blockName : cfg_.getBlocks()) {
            if (blockName == entry)
                continue;
            double fEntry = energy_.getFEntry(blockName);
            objective += fEntry * params_.E_epi * isRegionStart_[blockName];
        }
    }

    // Term 4: Checkpoint store cost
    // Sigma_d F_def[d] * store_enabled[d] * E_store[d]
    for (const auto &ds : state_.getDefSites()) {
        double fDef = energy_.getFDef(ds.id);
        double eStore = energy_.getEStore(ds.id);
        if (eStore > 0.0) {
            objective += fDef * eStore * storeEnabled_[ds.id];
        }
    }

    // Term 5: Expected restore cost
    // Sigma_b F_entry[b] * q_b * (
    //   Sigma_{r in LiveIn(b) cap Regs} E_rst[r] * is_region_start[b]
    //   + Sigma_{v in LiveIn(b) cap VMObjs} E_rst[v] * needs_vol_restore[b,v])
    for (const auto &blockName : cfg_.getBlocks()) {
        double fEntry = energy_.getFEntry(blockName);
        double qb = energy_.getQReboot(blockName);

        // SSA regs live-in: restore cost proportional to is_region_start
        const auto &regLiveIn = state_.getRegLiveIn(blockName);
        double totalRegRst = 0.0;
        for (llvm::Value *V : regLiveIn) {
            int elemId = state_.getRegStateElemId(V);
            if (elemId >= 0) {
                totalRegRst += energy_.getERst(static_cast<unsigned>(elemId));
            }
        }
        if (totalRegRst > 0.0) {
            objective +=
                fEntry * qb * totalRegRst * isRegionStart_[blockName];
        }

        // VMObjs live-in: restore cost proportional to needs_vol_restore
        const auto &vmObjLiveIn = state_.getVMObjLiveIn(blockName);
        for (llvm::GlobalVariable *GV : vmObjLiveIn) {
            int elemId = state_.getVMObjStateElemId(GV);
            if (elemId < 0)
                continue;
            auto key =
                std::make_pair(blockName, static_cast<unsigned>(elemId));
            auto it = needsVolRestore_.find(key);
            if (it == needsVolRestore_.end())
                continue;
            double eRst = energy_.getERst(static_cast<unsigned>(elemId));
            if (eRst > 0.0) {
                objective += fEntry * qb * eRst * it->second;
            }
        }
    }

    model_.setObjective(objective, GRB_MINIMIZE);
}

void CheckpointOptimizer::addConstraints() {
    addC1_EntryRegionStart();
    addC3_VMCapacity();
    addC4_NeedVolLinearization();
    addC5_CheckpointAvailability();
    addC6_EnergyInit();
    addC7_EnergyPropagation();
    addC8_BufferSafety();
}

// C1: Entry is always a region start
void CheckpointOptimizer::addC1_EntryRegionStart() {
    const std::string &entry = cfg_.getEntryBlock();
    if (!entry.empty() && isRegionStart_.count(entry)) {
        model_.addConstr(isRegionStart_[entry] == 1, "C1_entry_region_start");
    }
}

// C3: VM capacity constraint
// Sigma size(v) * placed_in_vm[v] <= S_VM
void CheckpointOptimizer::addC3_VMCapacity() {
    if (state_.getVMObjs().empty())
        return;

    GRBLinExpr vmUsage = 0;
    for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
        int elemId = state_.getVMObjStateElemId(GV);
        if (elemId < 0)
            continue;
        const StateElement &elem =
            state_.getStateElement(static_cast<unsigned>(elemId));
        vmUsage += static_cast<double>(elem.sizeBytes) * placedInVm_[GV];
    }
    model_.addConstr(vmUsage <= static_cast<double>(params_.vmCapacityBytes),
                     "C3_vm_capacity");
}

// C4: NeedVol linearization
// needs_vol_restore[b,v] <= is_region_start[b]
// needs_vol_restore[b,v] <= placed_in_vm[v]
// needs_vol_restore[b,v] >= is_region_start[b] + placed_in_vm[v] - 1
void CheckpointOptimizer::addC4_NeedVolLinearization() {
    for (auto &[key, yVar] : needsVolRestore_) {
        const std::string &blockName = key.first;
        unsigned elemId = key.second;
        const StateElement &elem = state_.getStateElement(elemId);
        llvm::GlobalVariable *GV = elem.globalVar;
        if (!GV)
            continue;

        std::string suffix =
            blockName + "_" + GV->getName().str();

        model_.addConstr(yVar <= isRegionStart_[blockName],
                         "C4a_nvr_le_x_" + suffix);
        model_.addConstr(yVar <= placedInVm_[GV],
                         "C4b_nvr_le_p_" + suffix);
        model_.addConstr(yVar >= isRegionStart_[blockName] +
                                     placedInVm_[GV] - 1,
                         "C4c_nvr_ge_xp_" + suffix);
    }
}

// C5: Checkpoint availability
// For SSA regs: store_enabled[d] >= is_region_start[b] for d in
// DefSites(b,r)
// For VMObjs: store_enabled[d] >= needs_vol_restore[b,v] for d in
// DefSites(b,v)
void CheckpointOptimizer::addC5_CheckpointAvailability() {
    unsigned constrIdx = 0;

    for (const auto &blockName : cfg_.getBlocks()) {
        // SSA regs live-in at b
        const auto &regLiveIn = state_.getRegLiveIn(blockName);
        for (llvm::Value *V : regLiveIn) {
            int elemId = state_.getRegStateElemId(V);
            if (elemId < 0)
                continue;
            unsigned eid = static_cast<unsigned>(elemId);
            const auto &reaching =
                state_.getReachingDefs(blockName, eid);
            for (unsigned dsId : reaching) {
                if (storeEnabled_.count(dsId)) {
                    model_.addConstr(
                        storeEnabled_[dsId] >= isRegionStart_[blockName],
                        "C5_reg_" + std::to_string(constrIdx++));
                }
            }
        }

        // VMObjs live-in at b
        const auto &vmObjLiveIn = state_.getVMObjLiveIn(blockName);
        for (llvm::GlobalVariable *GV : vmObjLiveIn) {
            int elemId = state_.getVMObjStateElemId(GV);
            if (elemId < 0)
                continue;
            unsigned eid = static_cast<unsigned>(elemId);
            auto nvrKey = std::make_pair(blockName, eid);
            auto nvrIt = needsVolRestore_.find(nvrKey);
            if (nvrIt == needsVolRestore_.end())
                continue;

            const auto &reaching =
                state_.getReachingDefs(blockName, eid);
            for (unsigned dsId : reaching) {
                if (storeEnabled_.count(dsId)) {
                    model_.addConstr(
                        storeEnabled_[dsId] >= nvrIt->second,
                        "C5_vmobj_" + std::to_string(constrIdx++));
                }
            }
        }
    }
}

// C6: Energy initialization at region starts
// energy_accumulated[b] >= E_start[b] - M*(1 - is_region_start[b])
// energy_accumulated[b] <= E_start[b] + M*(1 - is_region_start[b])
void CheckpointOptimizer::addC6_EnergyInit() {
    double M = params_.capacity;

    for (const auto &blockName : cfg_.getBlocks()) {
        GRBLinExpr eStart = buildEStart(blockName);

        model_.addConstr(
            energyAccumulated_[blockName] >=
                eStart - M * (1 - isRegionStart_[blockName]),
            "C6a_einit_lb_" + blockName);
        model_.addConstr(
            energyAccumulated_[blockName] <=
                eStart + M * (1 - isRegionStart_[blockName]),
            "C6b_einit_ub_" + blockName);
    }
}

// C7: Energy propagation along edges
// energy_accumulated[succ] >= energy_accumulated[pred] + E_blk[pred] -
//                             M * is_region_start[succ]
void CheckpointOptimizer::addC7_EnergyPropagation() {
    double M = params_.capacity;
    unsigned edgeIdx = 0;

    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);

        model_.addConstr(
            energyAccumulated_[dst] >=
                energyAccumulated_[src] + eBlkSrc -
                    M * isRegionStart_[dst],
            "C7_propagate_" + std::to_string(edgeIdx++));
    }
}

// C8: Buffer safety
// Per edge: energy_accumulated[pred] + E_blk[pred] +
//           E_epi * is_region_start[succ] <= E_buf
// For exits: energy_accumulated[pred] + E_blk[pred] <= E_buf
void CheckpointOptimizer::addC8_BufferSafety() {
    double Ebuf = params_.capacity;
    unsigned edgeIdx = 0;

    for (const auto &[src, dst] : cfg_.getEdges()) {
        GRBLinExpr eBlkSrc = buildEBlk(src);

        model_.addConstr(
            energyAccumulated_[src] + eBlkSrc +
                    params_.E_epi * isRegionStart_[dst] <=
                Ebuf,
            "C8_edge_" + std::to_string(edgeIdx++));
    }

    // Exit blocks
    for (const auto &exitBlock : cfg_.getExitBlocks()) {
        GRBLinExpr eBlkExit = buildEBlk(exitBlock);

        model_.addConstr(energyAccumulated_[exitBlock] + eBlkExit <= Ebuf,
                         "C8_exit_" + exitBlock);
    }
}

// E_blk[b] = E_base[b]
//           + Sigma_v E_nvm[b,v] * (1 - placed_in_vm[v])
//           + Sigma_{d in Defs(b)} E_store[d] * store_enabled[d]
GRBLinExpr CheckpointOptimizer::buildEBlk(const std::string &block) {
    GRBLinExpr expr = energy_.getEBase(block);

    // NVM penalties
    for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
        double eNvm = energy_.getENvm(block, GV);
        if (eNvm == 0.0)
            continue;
        // eNvm * (1 - p_v) = eNvm - eNvm * p_v
        expr += eNvm;
        expr -= eNvm * placedInVm_[GV];
    }

    // Store costs for defs in this block
    const auto &blockDefs = state_.getBlockDefSites(block);
    for (unsigned dsId : blockDefs) {
        double eStore = energy_.getEStore(dsId);
        if (eStore > 0.0) {
            expr += eStore * storeEnabled_[dsId];
        }
    }

    return expr;
}

// E_start[b] = (E_pro + Sigma_{r in LiveIn(b) cap Regs} E_rst[r])
//              * is_region_start[b]
//            + Sigma_{v in LiveIn(b) cap VMObjs} E_rst[v] *
//              needs_vol_restore[b,v]
GRBLinExpr CheckpointOptimizer::buildEStart(const std::string &block) {
    GRBLinExpr expr = 0;

    // Prologue + SSA reg restores (proportional to is_region_start)
    double prologuePlusRegRestore = params_.E_pro;
    const auto &regLiveIn = state_.getRegLiveIn(block);
    for (llvm::Value *V : regLiveIn) {
        int elemId = state_.getRegStateElemId(V);
        if (elemId >= 0) {
            prologuePlusRegRestore +=
                energy_.getERst(static_cast<unsigned>(elemId));
        }
    }
    expr += prologuePlusRegRestore * isRegionStart_[block];

    // VMObj restores (proportional to needs_vol_restore)
    const auto &vmObjLiveIn = state_.getVMObjLiveIn(block);
    for (llvm::GlobalVariable *GV : vmObjLiveIn) {
        int elemId = state_.getVMObjStateElemId(GV);
        if (elemId < 0)
            continue;
        auto key = std::make_pair(block, static_cast<unsigned>(elemId));
        auto it = needsVolRestore_.find(key);
        if (it == needsVolRestore_.end())
            continue;
        double eRst = energy_.getERst(static_cast<unsigned>(elemId));
        expr += eRst * it->second;
    }

    return expr;
}

void CheckpointOptimizer::extractSolution() {
    // Region starts
    for (const auto &[blockName, var] : isRegionStart_) {
        if (var.get(GRB_DoubleAttr_X) > 0.5) {
            solution_.regionStarts.insert(blockName);
        }
    }

    // Enabled def stores
    for (const auto &[dsId, var] : storeEnabled_) {
        if (var.get(GRB_DoubleAttr_X) > 0.5) {
            solution_.enabledDefStores.insert(dsId);
        }
    }

    // VM placement
    for (const auto &[gv, var] : placedInVm_) {
        solution_.vmPlacement[gv] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    // NeedVol restore
    for (const auto &[key, var] : needsVolRestore_) {
        solution_.needVolRestore[key] = var.get(GRB_DoubleAttr_X) > 0.5;
    }

    // Energy levels
    for (const auto &[blockName, var] : energyAccumulated_) {
        solution_.energyAccumulated[blockName] = var.get(GRB_DoubleAttr_X);
    }

    solution_.objectiveValue = model_.get(GRB_DoubleAttr_ObjVal);
}

} // namespace checkpoint
