#pragma once

#include "common/CFGAnalysis.h"
#include "milp/BBFreqLoader.h"
#include "milp/StateAnalysis.h"

#include "llvm/IR/Value.h"

#include <map>
#include <optional>
#include <string>

namespace checkpoint {

/// Energy parameters loaded from MILP config JSON.
/// Core energy fields are required; loop strip-mining toggle is optional.
struct MILPEnergyParams {
    double capacity;                     // E_buf: energy buffer capacity
    double E_pro;                        // Prologue energy at region boundary
    double E_epi;                        // Epilogue energy at region boundary
    unsigned N_reg = 16;                 // Number of registers (shared field)
    double regStoreEnergy;               // Energy to store one SSA reg to FRAM
    double regRestoreEnergy;             // Energy to restore one SSA reg from FRAM
    double nvmAccessPenalty;             // Extra energy per NVM access vs VM
    double memStoreEnergyPerByte;        // Energy per byte for VM->FRAM copy
    double memRestoreEnergyPerByte;      // Energy per byte for FRAM->VM copy
    unsigned vmCapacityBytes;            // VM (SRAM) capacity in bytes
    double loopStripMiningCost = 0.0;    // Optional strip-mining per-iteration control overhead
    bool loopStripMiningEnabled = false; // Enable loop strip-mining pass
};

/// Computes all energy parameters needed by the MILP (spec Sections 4 + 8).
///
/// Uses exact block visit counts from BBFreqLoader (produced by the
/// bb-freq-collect pipeline) and StateAnalysis access maps for per-global
/// NVM access penalty computation.
class EnergyModel {
  public:
    EnergyModel(const CFGAnalysis &cfg, const StateAnalysis &state, const BBFreqLoader &freqLoader,
                llvm::Function &F, const MILPEnergyParams &params);

    /// Get the energy parameters.
    const MILPEnergyParams &getParams() const { return params_; }

    /// E_base[b]: base energy cost of block b (from estimator, same as
    /// BlockInfo::energyCost).
    double getEBase(const llvm::BasicBlock *BB) const;

    /// E_nvm[b,v]: NVM access penalty for global v in block b.
    double getENvm(const llvm::BasicBlock *BB, llvm::GlobalVariable *gv) const;

    /// E_sv[v]: energy cost of committing variable v to checkpoint.
    double getESave(llvm::Value *v) const;

    /// E_rst[v]: energy cost of restoring variable v.
    double getERestore(llvm::Value *v) const;

    /// F_entry[b]: normalized profile entry frequency for block b.
    double getFEntry(const llvm::BasicBlock *BB) const;

  private:
    const CFGAnalysis &cfg_;
    const StateAnalysis &state_;
    const MILPEnergyParams &params_;

    // Precomputed values
    llvm::DenseMap<const llvm::BasicBlock *, double> fEntry_;
    std::map<std::pair<const llvm::BasicBlock *, llvm::GlobalVariable *>, double> eNvm_;
    std::map<llvm::Value *, double> eSaveByVar_;
    std::map<llvm::Value *, double> eRestoreByVar_;

    void computeFrequenciesFromFile(const BBFreqLoader &freqLoader, llvm::Function &F);
    void computeNvmPenalties();
    void computeSaveRestoreCosts();
};

/// Parse MILP energy parameters from a JSON config file.
/// Energy fields are required; loop_strip_mining_enabled is optional.
/// Returns std::nullopt on error.
std::optional<MILPEnergyParams> parseMILPEnergyParams(const std::string &configPath);

} // namespace checkpoint
