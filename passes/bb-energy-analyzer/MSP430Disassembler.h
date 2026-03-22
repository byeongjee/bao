#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bbanalyzer {

/// Represents a single disassembled instruction
struct Instruction {
    uint64_t address;
    unsigned size;          // in bytes
    std::string mnemonic;   // e.g., "mov", "add", "jmp"
    std::string operands;   // e.g., "r12, r13" or "#5, r12"
    std::string addrMode;   // e.g., "register_register", "immediate_indexed"
    std::string callTarget; // resolved function name for call instructions (from relocations)
};

/// MSP430-specific disassembler using LLVM's MC layer
class MSP430Disassembler {
  public:
    MSP430Disassembler();
    ~MSP430Disassembler();

    /// Disassemble MSP430 ELF/object file
    /// @param elfPath Path to .elf or .o file
    /// @return List of instructions with addresses
    std::vector<Instruction> disassemble(const std::string &elfPath);

  private:
    /// Determine addressing mode from operands
    /// @param mnemonic Instruction mnemonic
    /// @param operands Operand string
    /// @return Addressing mode string for energy lookup
    std::string determineAddressingMode(const std::string &mnemonic, const std::string &operands);

    /// Parse a single operand to determine its addressing mode component
    /// @param operand Single operand string (trimmed)
    /// @return Mode string: "register", "immediate", "indexed", "indirect",
    ///         "autoincrement", "absolute", "symbolic"
    std::string parseOperandMode(const std::string &operand);
};

} // namespace bbanalyzer
