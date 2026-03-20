#include "milp/EnergyModel.h"

#include "common/Logger.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#define JSON_NOEXCEPTION
#include <algorithm>
#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace checkpoint {

EnergyModel::EnergyModel(const CFGAnalysis &cfg, const StateAnalysis &state,
                         const BBFreqLoader &freqLoader, llvm::Function &F,
                         const MILPEnergyParams &params)
    : cfg_(cfg), state_(state), params_(params) {
    computeFrequenciesFromFile(freqLoader, F);
    computeNvmPenalties();
    computeSaveRestoreCosts();
}

double EnergyModel::getEBase(const llvm::BasicBlock *BB) const {
    return cfg_.getBlockInfo(BB).energyCost;
}

double EnergyModel::getENvm(const llvm::BasicBlock *BB, llvm::GlobalVariable *gv) const {
    auto key = std::make_pair(BB, gv);
    auto it = eNvm_.find(key);
    if (it != eNvm_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getESave(llvm::Value *v) const {
    auto it = eSaveByVar_.find(v);
    if (it != eSaveByVar_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getERestore(llvm::Value *v) const {
    auto it = eRestoreByVar_.find(v);
    if (it != eRestoreByVar_.end())
        return it->second;
    return 0.0;
}

double EnergyModel::getFEntry(const llvm::BasicBlock *BB) const {
    auto it = fEntry_.find(BB);
    if (it != fEntry_.end())
        return it->second;
    return 1.0;
}

// ---- Private implementation ----

void EnergyModel::computeFrequenciesFromFile(const BBFreqLoader &freqLoader, llvm::Function &F) {
    // Get entry block count from the frequency file.
    llvm::BasicBlock &Entry = F.getEntryBlock();
    auto entryCountOpt = freqLoader.getBlockCount(&Entry);
    if (!entryCountOpt) {
        llvm::report_fatal_error(llvm::Twine("MILP: entry block frequency missing for function '") +
                                     F.getName() + "' in BB frequency file",
                                 /*gen_crash_diag=*/false);
    }
    const uint64_t entryCount = std::max<uint64_t>(*entryCountOpt, 1);

    // Collect reachable blocks to distinguish truly missing from unreachable.
    llvm::SmallPtrSet<const llvm::BasicBlock *, 32> reachable;
    llvm::SmallVector<const llvm::BasicBlock *, 32> worklist;
    reachable.insert(&Entry);
    worklist.push_back(&Entry);
    while (!worklist.empty()) {
        const llvm::BasicBlock *BB = worklist.pop_back_val();
        for (const llvm::BasicBlock *succ : llvm::successors(BB)) {
            if (reachable.insert(succ).second)
                worklist.push_back(succ);
        }
    }

    for (llvm::BasicBlock &BB : F) {
        auto blockCountOpt = freqLoader.getBlockCount(&BB);
        if (!blockCountOpt) {
            if (reachable.contains(&BB)) {
                std::string blockName;
                llvm::raw_string_ostream os(blockName);
                BB.printAsOperand(os, /*PrintType=*/false);
                llvm::report_fatal_error(
                    llvm::Twine("MILP: missing BB frequency for reachable block ") + os.str() +
                        " in function '" + F.getName() + "'",
                    /*gen_crash_diag=*/false);
            }
            // Unreachable block — assign frequency 0.
            fEntry_[&BB] = 0.0;
            continue;
        }

        // Normalize so entry block has frequency 1.0.
        double normalized = static_cast<double>(*blockCountOpt) / static_cast<double>(entryCount);
        fEntry_[&BB] = normalized;
    }
}

void EnergyModel::computeNvmPenalties() {
    // E_nvm[b,v] = (loads + stores to v in b) * nvm_access_penalty
    // NVM penalties only apply to globals (eligible + ineligible globals).
    auto computeForGV = [&](llvm::GlobalVariable *GV, const llvm::BasicBlock *BB) {
        unsigned loads = state_.getLoadCount(BB, GV);
        unsigned stores = state_.getStoreCount(BB, GV);
        if (loads + stores > 0) {
            double penalty = static_cast<double>(loads + stores) * params_.nvmAccessPenalty;
            eNvm_[std::make_pair(BB, GV)] = penalty;
        }
    };

    for (const llvm::BasicBlock *BB : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs())
            computeForGV(GV, BB);
    }
}

void EnergyModel::computeSaveRestoreCosts() {
    // E_sv[v], E_rst[v] for all tracked variables (elig + inelig).
    auto computeForVar = [&](llvm::Value *V) {
        if (auto *I = llvm::dyn_cast<llvm::Instruction>(V)) {
            if (!llvm::isa<llvm::AllocaInst>(I)) {
                // Cross-block SSA values represent register state.
                eSaveByVar_[V] = params_.regStoreEnergy;
                eRestoreByVar_[V] = params_.regRestoreEnergy;
                return;
            }
        }

        unsigned sizeBytes = state_.getVarSizeBytes(V);
        if (sizeBytes == 0)
            return;
        eSaveByVar_[V] = static_cast<double>(sizeBytes) * params_.memStoreEnergyPerByte;
        eRestoreByVar_[V] = static_cast<double>(sizeBytes) * params_.memRestoreEnergyPerByte;
    };

    for (llvm::GlobalVariable *GV : state_.getVMObjs())
        computeForVar(GV);
    for (llvm::Value *V : state_.getIneligibleObjs())
        computeForVar(V);
}

// ---- Config parsing ----

std::optional<MILPEnergyParams> parseMILPEnergyParams(const std::string &configPath) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        PLOGE << "Error: Cannot open MILP config file: " << configPath;
        return std::nullopt;
    }

    nlohmann::json config = nlohmann::json::parse(file, nullptr, false);
    if (config.is_discarded()) {
        PLOGE << "Error: JSON parse error in MILP config: " << configPath;
        return std::nullopt;
    }

    // Required shared fields at root level.
    const std::vector<std::string> requiredDouble = {"capacity",
                                                     "E_pro",
                                                     "E_epi",
                                                     "reg_store_energy",
                                                     "reg_restore_energy",
                                                     "nvm_access_penalty",
                                                     "mem_store_energy_per_byte",
                                                     "mem_restore_energy_per_byte"};

    for (const auto &field : requiredDouble) {
        if (!config.contains(field)) {
            PLOGE << "Error: Missing required field '" << field
                  << "' in MILP config: " << configPath;
            return std::nullopt;
        }
    }
    if (!config.contains("vm_capacity_bytes")) {
        PLOGE << "Error: Missing required field 'vm_capacity_bytes'"
              << " in MILP config: " << configPath;
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

    // N_reg: optional shared field (default 16).
    if (config.contains("N_reg"))
        params.N_reg = config["N_reg"].get<unsigned>();

    // MILP-specific fields: check "milp" section first, then root (backward compat).
    nlohmann::json milpSection;
    if (config.contains("milp") && config["milp"].is_object())
        milpSection = config["milp"];

    // loop_strip_mining_enabled
    params.loopStripMiningEnabled = false;
    auto readBool = [&](const std::string &key, bool &out) -> bool {
        // Check milp section first, then root.
        for (const auto *src : {&milpSection, &config}) {
            if (src->contains(key)) {
                if (!(*src)[key].is_boolean()) {
                    PLOGE << "Error: Field '" << key << "' must be boolean"
                          << " in MILP config: " << configPath;
                    return false;
                }
                out = (*src)[key].get<bool>();
                return true;
            }
        }
        return true; // not found, keep default
    };

    if (!readBool("loop_strip_mining_enabled", params.loopStripMiningEnabled))
        return std::nullopt;

    params.addDebugMarkers = false;
    if (!readBool("add_debug_markers", params.addDebugMarkers))
        return std::nullopt;

    return params;
}

} // namespace checkpoint
