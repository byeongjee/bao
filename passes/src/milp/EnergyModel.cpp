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
    computeSaveRestoreCosts();
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

double EnergyModel::getESave(llvm::GlobalVariable *gv) const {
    auto it = eSaveByGV_.find(gv);
    if (it != eSaveByGV_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getERestore(llvm::GlobalVariable *gv) const {
    auto it = eRestoreByGV_.find(gv);
    if (it != eRestoreByGV_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getFEntry(const std::string &block) const {
    auto it = fEntry_.find(block);
    if (it != fEntry_.end())
        return it->second;
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

void EnergyModel::computeSaveRestoreCosts() {
    // E_sv[v], E_rst[v] are modeled for candidate globals only.
    for (llvm::GlobalVariable *GV : state_.getVMObjs()) {
        int elemId = state_.getVMObjStateElemId(GV);
        if (elemId < 0)
            continue;

        const StateElement &elem =
            state_.getStateElement(static_cast<unsigned>(elemId));
        eSaveByGV_[GV] =
            static_cast<double>(elem.sizeBytes) * params_.memStoreEnergyPerByte;
        eRestoreByGV_[GV] =
            static_cast<double>(elem.sizeBytes) * params_.memRestoreEnergyPerByte;
    }
}

// ---- Config parsing ----

std::optional<MILPEnergyParams> parseMILPEnergyParams(const std::string &configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        llvm::errs() << "Error: Cannot open MILP config file: "
                     << configPath << "\n";
        return std::nullopt;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        llvm::errs() << "Error: JSON parse error in MILP config: " << configPath << "\n";
        return std::nullopt;
    }

    // All fields are required - flat JSON (no energy_parameters wrapper)
    const std::vector<std::string> requiredDouble = {
        "capacity", "E_pro", "E_epi",
        "reg_store_energy", "reg_restore_energy", "nvm_access_penalty",
        "mem_store_energy_per_byte", "mem_restore_energy_per_byte",
        "q_reboot_probability"
    };

    for (const auto &field : requiredDouble) {
        if (!config.contains(field)) {
            llvm::errs() << "Error: Missing required field '" << field
                         << "' in MILP config: " << configPath << "\n";
            return std::nullopt;
        }
    }
    if (!config.contains("vm_capacity_bytes")) {
        llvm::errs() << "Error: Missing required field 'vm_capacity_bytes'"
                     << " in MILP config: " << configPath << "\n";
        return std::nullopt;
    }

    MILPEnergyParams params;
    params.capacity = config["capacity"].get<double>();
    params.E_pro = config["E_pro"].get<double>();
    params.E_epi = config["E_epi"].get<double>();
    params.regStoreEnergy = config["reg_store_energy"].get<double>();
    params.regRestoreEnergy = config["reg_restore_energy"].get<double>();
    params.nvmAccessPenalty = config["nvm_access_penalty"].get<double>();
    params.memStoreEnergyPerByte = config["mem_store_energy_per_byte"].get<double>();
    params.memRestoreEnergyPerByte = config["mem_restore_energy_per_byte"].get<double>();
    params.vmCapacityBytes = config["vm_capacity_bytes"].get<unsigned>();
    params.qRebootProb = config["q_reboot_probability"].get<double>();

    return params;
}

} // namespace checkpoint
