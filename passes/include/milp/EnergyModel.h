#pragma once

#include "common/CFGAnalysis.h"
#include "milp/StateAnalysis.h"

#include "llvm/Analysis/BlockFrequencyInfo.h"

#include <map>
#include <string>

namespace checkpoint {

/// Energy parameters loaded from config JSON for the new MILP formulation.
struct MILPEnergyParams {
    double capacity = 100.0;       // E_buf: energy buffer capacity
    double E_pro = 0.0;            // Prologue energy at region boundary
    double E_epi = 0.0;            // Epilogue energy at region boundary
    double regStoreEnergy = 0.0;   // Energy to store one SSA reg to FRAM
    double regRestoreEnergy = 0.0; // Energy to restore one SSA reg from FRAM
    double nvmAccessPenalty = 0.0; // Extra energy per NVM access vs VM
    double memStoreEnergyPerByte = 0.0;   // Energy per byte for VM->FRAM copy
    double memRestoreEnergyPerByte = 0.0; // Energy per byte for FRAM->VM copy
    unsigned vmCapacityBytes = 2048;      // VM (SRAM) capacity in bytes
    double qRebootProb = 1.0;     // Probability of reboot at boundary
};

/// Computes all energy parameters needed by the MILP (spec Sections 4 + 8).
///
/// Uses BlockFrequencyInfo for block frequency estimation (replaces the crude
/// 10^loopDepth heuristic), and StateAnalysis access maps for per-global
/// NVM access penalty computation.
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

    /// E_store[d]: energy cost of checkpoint store at def site d.
    double getEStore(unsigned defSiteId) const;

    /// E_rst[s]: energy cost of restoring state element s.
    double getERst(unsigned stateElemId) const;

    /// F_entry[b]: estimated entry frequency for block b.
    double getFEntry(const std::string &block) const;

    /// F_def[d]: estimated frequency of def site d (= F_entry of its block).
    double getFDef(unsigned defSiteId) const;

    /// q_b: reboot probability at block b (currently uniform = q_reboot_prob).
    double getQReboot(const std::string &block) const;

private:
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const MILPEnergyParams &params_;

    // Precomputed values
    std::map<std::string, double> fEntry_;          // Block frequencies
    std::map<std::pair<std::string, llvm::GlobalVariable *>, double> eNvm_;
    std::map<unsigned, double> eStore_;             // DefSite id -> store energy
    std::map<unsigned, double> eRst_;               // StateElement id -> restore energy

    void computeFrequencies(llvm::BlockFrequencyInfo &BFI, llvm::Function &F);
    void computeNvmPenalties();
    void computeStoreCosts();
    void computeRestoreCosts();
};

/// Parse MILP energy parameters from a JSON config file.
/// Missing fields get sensible defaults (backward compatible with old configs).
MILPEnergyParams parseMILPEnergyParams(const std::string &configPath);

} // namespace checkpoint
