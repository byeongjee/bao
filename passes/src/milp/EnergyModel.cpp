#include "milp/EnergyModel.h"

#include "common/BlockUtils.h"

#include "llvm/Support/raw_ostream.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>

namespace checkpoint {

EnergyModel::EnergyModel(const CFGAnalysis &cfg,
                         const StateAnalysis &state,
                         llvm::BlockFrequencyInfo &BFI,
                         llvm::Function &F,
                         const MILPEnergyParams &params)
    : cfg_(cfg), state_(state), params_(params) {
    computeFrequencies(BFI, F);
    computeNvmPenalties();
    computeStoreCosts();
    computeRestoreCosts();
}

double EnergyModel::getEBase(const std::string &block) const {
    return cfg_.getBlockInfo(block).energyCost;
}

double EnergyModel::getENvm(const std::string &block,
                             llvm::GlobalVariable *gv) const {
    auto key = std::make_pair(block, gv);
    auto it = eNvm_.find(key);
    if (it != eNvm_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getEStore(unsigned defSiteId) const {
    auto it = eStore_.find(defSiteId);
    if (it != eStore_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getERst(unsigned stateElemId) const {
    auto it = eRst_.find(stateElemId);
    if (it != eRst_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getFEntry(const std::string &block) const {
    auto it = fEntry_.find(block);
    if (it != fEntry_.end())
        return it->second;
    return 1.0;
}

double EnergyModel::getFDef(unsigned defSiteId) const {
    const auto &defSites = state_.getDefSites();
    if (defSiteId < defSites.size()) {
        return getFEntry(defSites[defSiteId].blockName);
    }
    return 1.0;
}

double EnergyModel::getQReboot(const std::string &) const {
    return params_.qRebootProb;
}

// ---- Private implementation ----

void EnergyModel::computeFrequencies(llvm::BlockFrequencyInfo &BFI,
                                      llvm::Function &F) {
    // Use LLVM's BlockFrequencyInfo for much better estimates than
    // the crude 10^loopDepth heuristic.
    // BFI returns integer-scaled frequencies; we normalize by entry frequency.
    llvm::BasicBlock &Entry = F.getEntryBlock();
    uint64_t entryFreq = BFI.getBlockFreq(&Entry).getFrequency();
    if (entryFreq == 0)
        entryFreq = 1;

    for (llvm::BasicBlock &BB : F) {
        std::string blockName = getBlockName(BB, F);
        uint64_t freq = BFI.getBlockFreq(&BB).getFrequency();
        // Normalize so entry block has frequency 1.0
        double normalized = static_cast<double>(freq) /
                            static_cast<double>(entryFreq);
        // Ensure minimum frequency of 1.0 (entry-level)
        if (normalized < 1.0)
            normalized = 1.0;
        fEntry_[blockName] = normalized;
    }
}

void EnergyModel::computeNvmPenalties() {
    // E_nvm[b,v] = (loads + stores to v in b) * nvm_access_penalty
    for (const auto &blockName : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
            unsigned loads = state_.getLoadCount(blockName, GV);
            unsigned stores = state_.getStoreCount(blockName, GV);
            if (loads + stores > 0) {
                double penalty =
                    static_cast<double>(loads + stores) * params_.nvmAccessPenalty;
                eNvm_[std::make_pair(blockName, GV)] = penalty;
            }
        }
    }
}

void EnergyModel::computeStoreCosts() {
    // E_store[d]: SSA reg -> reg_store_energy, VMObj -> size * mem_store_energy_per_byte
    for (const auto &ds : state_.getDefSites()) {
        if (ds.kind == DefSite::SSAReg) {
            eStore_[ds.id] = params_.regStoreEnergy;
        } else {
            // MemoryDef: cost is based on the size of the VMObj
            int elemId = state_.getVMObjStateElemId(ds.globalVar);
            if (elemId >= 0) {
                const StateElement &elem = state_.getStateElement(
                    static_cast<unsigned>(elemId));
                eStore_[ds.id] =
                    static_cast<double>(elem.sizeBytes) *
                    params_.memStoreEnergyPerByte;
            }
        }
    }
}

void EnergyModel::computeRestoreCosts() {
    // E_rst[s]: SSA reg -> reg_restore_energy, VMObj -> size * mem_restore_energy_per_byte
    for (const auto &elem : state_.getStateElements()) {
        if (elem.kind == StateElement::Reg) {
            eRst_[elem.id] = params_.regRestoreEnergy;
        } else {
            eRst_[elem.id] =
                static_cast<double>(elem.sizeBytes) *
                params_.memRestoreEnergyPerByte;
        }
    }
}

// ---- Config parsing ----

MILPEnergyParams parseMILPEnergyParams(const std::string &configPath) {
    MILPEnergyParams params;

    std::ifstream file(configPath);
    if (!file.is_open()) {
        llvm::errs() << "Warning: Cannot open config for MILP params: "
                     << configPath << "\n";
        return params;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::errs() << "Warning: JSON parse error in: " << configPath << "\n";
        return params;
    }

    if (!config.contains("energy_parameters"))
        return params;

    const auto &ep = config["energy_parameters"];

    // capacity is already loaded by the estimator, but we read it here too
    // for the MILP's E_buf
    if (ep.contains("capacity"))
        params.capacity = ep["capacity"].get<double>();

    // New MILP-specific parameters with backward-compatible defaults
    if (ep.contains("E_pro"))
        params.E_pro = ep["E_pro"].get<double>();
    if (ep.contains("E_epi"))
        params.E_epi = ep["E_epi"].get<double>();
    if (ep.contains("reg_store_energy"))
        params.regStoreEnergy = ep["reg_store_energy"].get<double>();
    if (ep.contains("reg_restore_energy"))
        params.regRestoreEnergy = ep["reg_restore_energy"].get<double>();
    if (ep.contains("nvm_access_penalty"))
        params.nvmAccessPenalty = ep["nvm_access_penalty"].get<double>();
    if (ep.contains("mem_store_energy_per_byte"))
        params.memStoreEnergyPerByte =
            ep["mem_store_energy_per_byte"].get<double>();
    if (ep.contains("mem_restore_energy_per_byte"))
        params.memRestoreEnergyPerByte =
            ep["mem_restore_energy_per_byte"].get<double>();
    if (ep.contains("vm_capacity_bytes"))
        params.vmCapacityBytes = ep["vm_capacity_bytes"].get<unsigned>();
    if (ep.contains("q_reboot_probability"))
        params.qRebootProb = ep["q_reboot_probability"].get<double>();

    return params;
}

} // namespace checkpoint
