#pragma once

#include "EnergyModel.h"

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"

#include <string>

namespace checkpoint {

/// Computes per-MachineBasicBlock energy costs using MSP430 opcodes
/// and the existing EnergyModel from bb-energy-analyzer.
///
/// Maps LLVM opcode names (e.g., MOV16rr, ADD8ri) to assembly mnemonics
/// and addressing mode strings matching the energy config JSON format.
class MachineEnergyEstimator {
  public:
    explicit MachineEnergyEstimator(const std::string &configPath);

    /// Estimate total energy for a machine basic block
    double estimateBlock(const llvm::MachineBasicBlock &MBB) const;

    /// Estimate energy for a single machine instruction
    double estimateInstruction(const llvm::MachineInstr &MI) const;

  private:
    bbanalyzer::EnergyModel model_;

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
