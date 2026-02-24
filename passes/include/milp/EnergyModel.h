#pragma once

#include "common/CFGAnalysis.h"
#include "milp/StateAnalysis.h"

#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/IR/Value.h"

#include <map>
#include <optional>
#include <string>

namespace checkpoint {

/// Energy parameters loaded from MILP config JSON.
/// Core energy fields are required; loop strip-mining toggle is optional.
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
    bool loopStripMiningEnabled = false; // Enable loop strip-mining pass
    // Additional strip-mining safety margin as % of capacitor capacity.
    // Effective strip-mining budget is reduced by this amount.
    double loopStripMiningMarginPercent = 0.0;
    bool addDebugMarkers = false;    // Emit debug marker calls for register save/restore
};

/// Computes all energy parameters needed by the MILP (spec Sections 4 + 8).
///
/// Uses profile-guided block frequencies from BlockFrequencyInfo (real profile
/// counts required) and StateAnalysis access maps for per-global NVM access
/// penalty computation.
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
    double getEBase(const llvm::BasicBlock *BB) const;

    /// E_nvm[b,v]: NVM access penalty for global v in block b.
    double getENvm(const llvm::BasicBlock *BB,
                   llvm::GlobalVariable *gv) const;

    /// E_sv[v]: energy cost of committing variable v to checkpoint.
    double getESave(llvm::Value *v) const;

    /// E_rst[v]: energy cost of restoring variable v.
    double getERestore(llvm::Value *v) const;

    /// F_entry[b]: normalized profile entry frequency for block b.
    double getFEntry(const llvm::BasicBlock *BB) const;

    /// q_reboot: uniform reboot probability (= q_reboot_prob parameter).
    double getQReboot() const;

private:
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const MILPEnergyParams &params_;

    // Precomputed values
    llvm::DenseMap<const llvm::BasicBlock *, double> fEntry_;
    std::map<std::pair<const llvm::BasicBlock *, llvm::GlobalVariable *>, double> eNvm_;
    std::map<llvm::Value *, double> eSaveByVar_;
    std::map<llvm::Value *, double> eRestoreByVar_;

    void computeFrequencies(llvm::BlockFrequencyInfo &BFI, llvm::Function &F);
    void computeNvmPenalties();
    void computeSaveRestoreCosts();
};

/// Parse MILP energy parameters from a JSON config file.
/// Energy fields are required; loop_strip_mining_enabled and
/// loop_strip_mining_margin_percent are optional.
/// Returns std::nullopt on error.
std::optional<MILPEnergyParams> parseMILPEnergyParams(const std::string &configPath);

} // namespace checkpoint
