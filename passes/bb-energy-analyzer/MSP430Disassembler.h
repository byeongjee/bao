#pragma once

#include <cstdint>
#include <map>
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

    /// Number of stack memory accesses an instruction performs: operands
    /// addressed through SP (r1) or, when *fpIsR4*, the frame pointer (r4),
    /// plus the implicit push/pop of push, pop, call, ret and pushm/popm.
    static unsigned countStackAccesses(const Instruction &instr, bool fpIsR4);

    /// Parse instruction and relocation lines from msp430-elf-objdump -d -r output.
    /// Exposed separately so parsing can be tested without invoking the external tool.
    /// Mnemonic width suffixes (e.g., .b/.w) are normalized away because the
    /// energy model is keyed by the base mnemonic and addressing mode.
    static std::vector<Instruction> parseObjdumpOutput(const std::string &objdumpOutput);

  private:
    /// Determine addressing mode from operands
    /// @param mnemonic Instruction mnemonic
    /// @param operands Operand string
    /// @return Addressing mode string for energy lookup
    static std::string determineAddressingMode(const std::string &mnemonic,
                                               const std::string &operands);

    /// Parse a single operand to determine its addressing mode component
    /// @param operand Single operand string (trimmed)
    /// @return Mode string: "register", "immediate", "indexed", "indirect",
    ///         "autoincrement", "absolute", "symbolic"
    static std::string parseOperandMode(const std::string &operand);

    /// Parse function labels from objdump output into offset->name map.
    /// Matches lines like: 00000000 <timing_gpio_init>:
    static std::map<uint64_t, std::string> parseFunctionLabels(const std::string &objdumpOutput);

    /// Resolve section-relative call targets (e.g., .text+0x4a) to function
    /// names using the offset->name map from parseFunctionLabels().
    static void resolveCallTargets(std::vector<Instruction> &instructions,
                                   const std::map<uint64_t, std::string> &offsetToFunc);
};

} // namespace bbanalyzer
