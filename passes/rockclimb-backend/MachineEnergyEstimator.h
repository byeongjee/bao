#pragma once

#include "EnergyModel.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace checkpoint {

/// Computes per-MachineBasicBlock energy costs using MSP430 opcodes
/// and the existing EnergyModel from bb-energy-analyzer.
///
/// Two modes:
/// 1. Instruction-level estimation: maps LLVM opcode names to assembly mnemonics
///    and addressing mode strings matching the energy config JSON format.
/// 2. Pre-computed: loads per-BB energy from bb-energy-analyzer output JSON.
class MachineEnergyEstimator {
  public:
    explicit MachineEnergyEstimator(const std::string &configPath);

    /// Load pre-computed per-BB energy from bb-energy-analyzer output JSON.
    /// Falls back to instruction-level estimation for blocks not in the data.
    static std::unique_ptr<MachineEnergyEstimator>
    fromPrecomputed(const std::string &energyDataPath);

    /// Estimate total energy for a machine basic block
    double estimateBlock(const llvm::MachineBasicBlock &MBB) const;

    /// Estimate energy for a single machine instruction
    double estimateInstruction(const llvm::MachineInstr &MI) const;

    /// Collect all energy parameter keys required by the given function.
    /// Returns a sorted set of keys (e.g., "mov_register_register").
    /// Keys missing from the config are marked in the second set.
    void collectRequiredKeys(const llvm::MachineFunction &MF, std::set<std::string> &allKeys,
                             std::set<std::string> &missingKeys) const;

    /// Get the energy key for a single instruction (for diagnostics)
    std::string getInstructionKey(const llvm::MachineInstr &MI) const;

    /// Whether this estimator uses pre-computed BB energy data
    bool isPrecomputed() const { return usePrecomputed_; }

  private:
    /// Private constructor for pre-computed mode (no EnergyModel config needed)
    MachineEnergyEstimator();

    bbanalyzer::EnergyModel model_;
    bool usePrecomputed_ = false;

    /// Pre-computed per-BB energy: functionName -> (bbName -> energy)
    std::unordered_map<std::string, std::unordered_map<std::string, double>> precomputedEnergy_;

    /// Lookup pre-computed energy for a MBB. Returns negative if not found.
    double lookupPrecomputed(const llvm::MachineBasicBlock &MBB) const;

    /// Extract base mnemonic from LLVM opcode name (e.g., "MOV16rr" → "mov")
    static std::string extractMnemonic(llvm::StringRef opName);

    /// Extract addressing mode suffix from LLVM opcode name (e.g., "MOV16rr" → "rr")
    static std::string extractSuffix(llvm::StringRef opName);

    /// Map a single suffix character to an addressing mode string,
    /// using MachineOperand details to distinguish memory sub-modes.
    static std::string mapSuffixChar(char c, const llvm::MachineInstr &MI, unsigned operandIdx);

    /// Build combined addressing mode string from instruction suffix.
    /// Format: "srcMode_dstMode" for two-operand, "mode" for single-operand.
    std::string getAddressingMode(const llvm::MachineInstr &MI, llvm::StringRef suffix) const;

    /// Classify a memory operand as "indexed", "symbolic", or "absolute"
    static std::string classifyMemoryOperand(const llvm::MachineInstr &MI, unsigned startIdx);
};

} // namespace checkpoint
