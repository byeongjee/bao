#pragma once

#include "common/CFGAnalysis.h"
#include "milp/StateAnalysis.h"

#include "llvm/Analysis/BlockFrequencyInfo.h"

#include <map>
#include <optional>
#include <string>

namespace checkpoint {

/// Energy parameters loaded from MILP config JSON.
/// Core energy fields are required; loop chunking toggle is optional.
struct MILPEnergyParams {
    double capacity;               // E_buf: energy buffer capacity
    double E_pro;                  // Prologue energy at region boundary
    double E_epi;                  // Epilogue energy at region boundary
    double regStoreEnergy;         // Energy to store one SSA reg to FRAM
    double regRestoreEnergy;       // Energy to restore one SSA reg from FRAM
    double nvmAccessPenalty;       // Extra energy per NVM access vs VM
    double memStoreEnergyPerByte;  // Energy per byte for VM->FRAM copy
    double memRestoreEnergyPerByte; // Energy per byte for FRAM->VM copy
    unsigned vmCapacityBytes;      // VM (SRAM) capacity in bytes
    double qRebootProb;            // Probability of reboot at boundary
    bool loopChunkingEnabled = false; // Enable loop chunking pass
};

/// Computes all energy parameters needed by the MILP (spec Sections 4 + 8).
///
/// Uses BlockFrequencyInfo for block frequency estimation and StateAnalysis
/// access maps for per-global NVM access penalty computation.
class EnergyModel {
public:
    EnergyModel(const CFGAnalysis &cfg,
                const StateAnalysis &state,
                llvm::BlockFrequencyInfo &BFI,
                llvm::Function &F,
                const MILPEnergyParams &params);

    /// Get the energy parameters.
    const MILPEnergyParams &getParams() const { return params_; }

    /// E_base[b]: base energy cost of block b (from estimator, same as
    /// BlockInfo::energyCost).
    double getEBase(const std::string &block) const;

    /// E_nvm[b,v]: NVM access penalty for global v in block b.
    double getENvm(const std::string &block,
                   llvm::GlobalVariable *gv) const;

    /// E_sv[v]: energy cost of committing candidate global v to checkpoint.
    double getESave(llvm::GlobalVariable *gv) const;

    /// E_rst[v]: energy cost of restoring candidate global v.
    double getERestore(llvm::GlobalVariable *gv) const;

    /// F_entry[b]: estimated entry frequency for block b.
    double getFEntry(const std::string &block) const;

    /// q_reboot: uniform reboot probability (= q_reboot_prob parameter).
    double getQReboot() const;

private:
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const MILPEnergyParams &params_;

    // Precomputed values
    std::map<std::string, double> fEntry_;          // Block frequencies
    std::map<std::pair<std::string, llvm::GlobalVariable *>, double> eNvm_;
    std::map<llvm::GlobalVariable *, double> eSaveByGV_;
    std::map<llvm::GlobalVariable *, double> eRestoreByGV_;

    void computeFrequencies(llvm::BlockFrequencyInfo &BFI, llvm::Function &F);
    void computeNvmPenalties();
    void computeSaveRestoreCosts();
};

/// Parse MILP energy parameters from a JSON config file.
/// Energy fields are required; loop_chunking_enabled is optional.
/// Returns std::nullopt on error.
std::optional<MILPEnergyParams> parseMILPEnergyParams(const std::string &configPath);

} // namespace checkpoint
