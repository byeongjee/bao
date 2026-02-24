#include "milp/EnergyModel.h"

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
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

double EnergyModel::getEBase(const llvm::BasicBlock *BB) const {
    return cfg_.getBlockInfo(BB).energyCost;
}

double EnergyModel::getENvm(const llvm::BasicBlock *BB,
                             llvm::GlobalVariable *gv) const {
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

double EnergyModel::getQReboot() const {
    return params_.qRebootProb;
}

// ---- Private implementation ----

void EnergyModel::computeFrequencies(llvm::BlockFrequencyInfo &BFI,
                                      llvm::Function &F) {
    // Use profile-backed block counts and normalize by entry count.
    // Unlike the old heuristic path, we do not clamp cold blocks to 1.0.
    llvm::BasicBlock &Entry = F.getEntryBlock();
    auto entryCountOpt = BFI.getBlockProfileCount(&Entry, /*AllowSynthetic=*/false);
    if (!entryCountOpt) {
        llvm::report_fatal_error(
            llvm::Twine("MILP requires real profile count for entry block in function '") +
                F.getName() + "'",
            /*GenCrashDiag=*/false);
    }
    const uint64_t entryCount = std::max<uint64_t>(*entryCountOpt, 1);

    for (llvm::BasicBlock &BB : F) {
        auto blockCountOpt =
            BFI.getBlockProfileCount(&BB, /*AllowSynthetic=*/false);
        if (!blockCountOpt) {
            std::string blockName;
            llvm::raw_string_ostream os(blockName);
            BB.printAsOperand(os, /*PrintType=*/false);
            llvm::report_fatal_error(
                llvm::Twine("MILP missing profile count for block ") + os.str() +
                    " in function '" + F.getName() + "'",
                /*GenCrashDiag=*/false);
        }

        // Normalize so entry block has frequency 1.0.
        // Truly cold blocks are allowed to be below 1.0.
        double normalized = static_cast<double>(*blockCountOpt) /
                            static_cast<double>(entryCount);
        fEntry_[&BB] = normalized;
    }
}

void EnergyModel::computeNvmPenalties() {
    // E_nvm[b,v] = (loads + stores to v in b) * nvm_access_penalty
    // NVM penalties only apply to globals (eligible + ineligible globals).
    auto computeForGV = [&](llvm::GlobalVariable *GV,
                            const llvm::BasicBlock *BB) {
        unsigned loads = state_.getLoadCount(BB, GV);
        unsigned stores = state_.getStoreCount(BB, GV);
        if (loads + stores > 0) {
            double penalty =
                static_cast<double>(loads + stores) * params_.nvmAccessPenalty;
            eNvm_[std::make_pair(BB, GV)] = penalty;
        }
    };

    // Collect ineligible globals for NVM penalty computation.
    std::vector<llvm::GlobalVariable *> ineligGlobals;
    for (llvm::Value *V : state_.getIneligibleObjs()) {
        if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V))
            ineligGlobals.push_back(GV);
    }

    for (const llvm::BasicBlock *BB : cfg_.getBlocks()) {
        for (llvm::GlobalVariable *GV : state_.getVMObjs())
            computeForGV(GV, BB);
        for (llvm::GlobalVariable *GV : ineligGlobals)
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
        eSaveByVar_[V] =
            static_cast<double>(sizeBytes) * params_.memStoreEnergyPerByte;
        eRestoreByVar_[V] =
            static_cast<double>(sizeBytes) * params_.memRestoreEnergyPerByte;
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
    params.loopStripMiningEnabled = false;
    if (config.contains("loop_strip_mining_enabled")) {
        if (!config["loop_strip_mining_enabled"].is_boolean()) {
            llvm::errs() << "Error: Field 'loop_strip_mining_enabled' must be boolean"
                         << " in MILP config: " << configPath << "\n";
            return std::nullopt;
        }
        params.loopStripMiningEnabled = config["loop_strip_mining_enabled"].get<bool>();
    }
    params.loopStripMiningMarginPercent = 0.0;
    if (config.contains("loop_strip_mining_margin_percent")) {
        if (!config["loop_strip_mining_margin_percent"].is_number()) {
            llvm::errs()
                << "Error: Field 'loop_strip_mining_margin_percent' must be numeric"
                << " in MILP config: " << configPath << "\n";
            return std::nullopt;
        }
        double marginPercent = config["loop_strip_mining_margin_percent"].get<double>();
        if (!std::isfinite(marginPercent) || marginPercent < 0.0 ||
            marginPercent > 100.0) {
            llvm::errs()
                << "Error: Field 'loop_strip_mining_margin_percent' must be in [0, 100]"
                << " in MILP config: " << configPath << "\n";
            return std::nullopt;
        }
        params.loopStripMiningMarginPercent = marginPercent;
    }
    params.addDebugMarkers = false;
    if (config.contains("add_debug_markers")) {
        if (!config["add_debug_markers"].is_boolean()) {
            llvm::errs() << "Error: Field 'add_debug_markers' must be boolean"
                         << " in MILP config: " << configPath << "\n";
            return std::nullopt;
        }
        params.addDebugMarkers = config["add_debug_markers"].get<bool>();
    }

    return params;
}

} // namespace checkpoint
