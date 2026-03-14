#include "MachineEnergyEstimator.h"

#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/raw_ostream.h"

#include "common/Logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace llvm;

namespace checkpoint {

MachineEnergyEstimator::MachineEnergyEstimator(const std::string &configPath)
    : model_(configPath) {}

MachineEnergyEstimator::MachineEnergyEstimator() : model_("") {}

std::unique_ptr<MachineEnergyEstimator>
MachineEnergyEstimator::fromPrecomputed(const std::string &energyDataPath) {
    std::ifstream file(energyDataPath);
    if (!file.is_open()) {
        PLOGE << "Error: Cannot open pre-computed energy data: " << energyDataPath;
        return nullptr;
    }

    std::stringstream buf;
    buf << file.rdbuf();

    nlohmann::json data = nlohmann::json::parse(buf.str(), nullptr, false);
    if (data.is_discarded()) {
        PLOGE << "Error: JSON parse error in energy data: " << energyDataPath;
        return nullptr;
    }

    auto estimator = std::unique_ptr<MachineEnergyEstimator>(new MachineEnergyEstimator());
    estimator->usePrecomputed_ = true;

    if (!data.contains("functions") || !data["functions"].is_object()) {
        PLOGE << "Error: Energy data missing 'functions' object";
        return nullptr;
    }

    for (auto &[funcName, funcData] : data["functions"].items()) {
        if (!funcData.contains("bb_energy") || !funcData["bb_energy"].is_object())
            continue;
        auto &funcMap = estimator->precomputedEnergy_[funcName];
        for (auto &[bbName, bbData] : funcData["bb_energy"].items()) {
            if (bbData.contains("energy") && bbData["energy"].is_number())
                funcMap[bbName] = bbData["energy"].get<double>();
        }
    }

    PLOGI << "Loaded pre-computed BB energy for " << estimator->precomputedEnergy_.size()
          << " function(s) from " << energyDataPath;
    return estimator;
}

double MachineEnergyEstimator::lookupPrecomputed(const MachineBasicBlock &MBB) const {
    const MachineFunction *MF = MBB.getParent();
    std::string funcName = MF->getName().str();

    auto funcIt = precomputedEnergy_.find(funcName);
    if (funcIt == precomputedEnergy_.end())
        return -1.0;

    // Try the MBB name first
    std::string bbName;
    if (MBB.hasName()) {
        bbName = MBB.getName().str();
    } else {
        // Match assign-bb-debuginfo convention: "bb" + 0-based index
        bbName = "bb" + std::to_string(MBB.getNumber());
    }

    auto bbIt = funcIt->second.find(bbName);
    if (bbIt != funcIt->second.end())
        return bbIt->second;

    return -1.0;
}

double MachineEnergyEstimator::estimateBlock(const MachineBasicBlock &MBB) const {
    if (usePrecomputed_) {
        double energy = lookupPrecomputed(MBB);
        if (energy >= 0.0)
            return energy;
        // Fall through to instruction-level estimation if not found
        // (only works if model_ was loaded with a config)
    }

    double total = 0.0;
    for (const MachineInstr &MI : MBB) {
        total += estimateInstruction(MI);
    }
    return total;
}

double MachineEnergyEstimator::estimateInstruction(const MachineInstr &MI) const {
    // Skip pseudo-instructions and debug values
    if (MI.isPseudo() || MI.isDebugInstr() || MI.isImplicitDef() || MI.isKill() || MI.isLabel())
        return 0.0;

    const TargetInstrInfo *TII = MI.getParent()->getParent()->getSubtarget().getInstrInfo();
    StringRef opName = TII->getName(MI.getOpcode());

    std::string mnemonic = extractMnemonic(opName);
    std::string suffix = extractSuffix(opName);
    std::string addrMode = getAddressingMode(MI, suffix);

    return model_.getEnergy(mnemonic, addrMode);
}

// Extract base mnemonic from LLVM opcode name.
// Patterns:
//   "MOV16rr"  → "mov"    (strip size + suffix)
//   "ADD8ri"   → "add"
//   "JMP"      → "jmp"    (no size/suffix)
//   "RET"      → "ret"
//   "CALLi"    → "call"
//   "PUSH16r"  → "push"
//   "RLAM16"   → "rlam"
//   "NOP"      → "nop"
std::string MachineEnergyEstimator::extractMnemonic(StringRef opName) {
    // Find where the uppercase mnemonic ends.
    // MSP430 opcode names: uppercase mnemonic, then optional size (8/16),
    // then optional lowercase addressing mode suffix.
    size_t i = 0;

    // Consume uppercase letters (the base mnemonic)
    while (i < opName.size() && std::isupper(static_cast<unsigned char>(opName[i])))
        ++i;

    std::string mnemonic = opName.substr(0, i).lower();
    return mnemonic;
}

// Extract addressing mode suffix from LLVM opcode name.
// Returns the lowercase suffix characters after the size digit(s).
// "MOV16rr" → "rr", "ADD8ri" → "ri", "JMP" → "", "CALLi" → "i"
std::string MachineEnergyEstimator::extractSuffix(StringRef opName) {
    size_t i = 0;

    // Skip uppercase mnemonic
    while (i < opName.size() && std::isupper(static_cast<unsigned char>(opName[i])))
        ++i;

    // Skip size digits (8, 16)
    while (i < opName.size() && std::isdigit(static_cast<unsigned char>(opName[i])))
        ++i;

    // Remaining characters are the suffix
    return opName.substr(i).str();
}

// Map a single suffix character to an addressing mode string.
// r=register, i=immediate, c=immediate(constant), m=memory,
// n=indirect, p=autoincrement
std::string MachineEnergyEstimator::mapSuffixChar(char c, const MachineInstr &MI,
                                                  unsigned operandIdx) {
    switch (c) {
    case 'r':
        return "register";
    case 'i': {
        // Check for special immediate values (1, 2, 4, 8) used by constant
        // generator in MSP430
        if (operandIdx < MI.getNumOperands() && MI.getOperand(operandIdx).isImm()) {
            int64_t val = MI.getOperand(operandIdx).getImm();
            if (val == 1 || val == 2 || val == 4 || val == 8)
                return "immediate_" + std::to_string(val);
        }
        return "immediate";
    }
    case 'c':
        // 6-bit constant — still an immediate for energy purposes
        return "immediate";
    case 'm':
        return classifyMemoryOperand(MI, operandIdx);
    case 'n':
        return "indirect";
    case 'p':
        return "autoincrement";
    default:
        return "register"; // Conservative fallback
    }
}

// Classify a memory operand as indexed, symbolic, or absolute.
// In MSP430 MIR, memory operands are typically:
//   - reg + offset → "indexed"
//   - global/symbol → "symbolic"
//   - absolute address → "absolute"
std::string MachineEnergyEstimator::classifyMemoryOperand(const MachineInstr &MI,
                                                          unsigned startIdx) {
    // Scan operands from startIdx looking for the memory operand components
    for (unsigned i = startIdx; i < MI.getNumOperands(); ++i) {
        const MachineOperand &MO = MI.getOperand(i);
        if (MO.isGlobal() || MO.isSymbol() || MO.isCPI() || MO.isBlockAddress() || MO.isJTI())
            return "symbolic";
        if (MO.isFI())
            return "indexed"; // Frame index = stack-relative = indexed
    }
    // Default: indexed addressing (reg + imm offset)
    return "indexed";
}

// Build combined addressing mode string.
// LLVM MSP430 suffix convention: first char = dst mode, second char = src mode.
// Energy config format: "srcMode_dstMode" (assembly order: op src, dst).
// Single-operand: just "mode". No suffix: empty string.
std::string MachineEnergyEstimator::getAddressingMode(const MachineInstr &MI,
                                                      StringRef suffix) const {
    if (suffix.empty())
        return "";

    if (suffix.size() == 1) {
        // Single operand instruction (PUSH, POP, RRA, etc.)
        // or branch/call with one operand
        return mapSuffixChar(suffix[0], MI, 0);
    }

    if (suffix.size() == 2) {
        // Two-operand instruction.
        // LLVM suffix: [dstMode][srcMode]
        // Energy config: srcMode_dstMode (assembly order)
        char dstChar = suffix[0];
        char srcChar = suffix[1];

        // Find the approximate operand indices.
        // Destination operands come first in LLVM MI, source operands follow.
        // For 'r' dst: operand 0 is dst reg
        // For 'm' dst: operands 0-1 are dst mem (base reg + offset)
        unsigned srcOpIdx = (dstChar == 'm') ? 2 : 1;

        std::string srcMode = mapSuffixChar(srcChar, MI, srcOpIdx);
        std::string dstMode = mapSuffixChar(dstChar, MI, 0);

        return srcMode + "_" + dstMode;
    }

    // Longer suffix (e.g., RLAM has "immediate_N_register" pattern)
    // Fall through with empty mode — will use default energy
    return "";
}

std::string MachineEnergyEstimator::getInstructionKey(const MachineInstr &MI) const {
    if (MI.isPseudo() || MI.isDebugInstr() || MI.isImplicitDef() || MI.isKill() || MI.isLabel())
        return "";

    const TargetInstrInfo *TII = MI.getParent()->getParent()->getSubtarget().getInstrInfo();
    StringRef opName = TII->getName(MI.getOpcode());

    std::string mnemonic = extractMnemonic(opName);
    std::string suffix = extractSuffix(opName);
    std::string addrMode = getAddressingMode(MI, suffix);

    if (addrMode.empty())
        return mnemonic;
    return mnemonic + "_" + addrMode;
}

void MachineEnergyEstimator::collectRequiredKeys(const MachineFunction &MF,
                                                 std::set<std::string> &allKeys,
                                                 std::set<std::string> &missingKeys) const {
    for (const MachineBasicBlock &MBB : MF) {
        for (const MachineInstr &MI : MBB) {
            if (MI.isPseudo() || MI.isDebugInstr() || MI.isImplicitDef() || MI.isKill() ||
                MI.isLabel())
                continue;

            const TargetInstrInfo *TII = MI.getParent()->getParent()->getSubtarget().getInstrInfo();
            StringRef opName = TII->getName(MI.getOpcode());

            std::string mnemonic = extractMnemonic(opName);
            std::string suffix = extractSuffix(opName);
            std::string addrMode = getAddressingMode(MI, suffix);

            std::string key = addrMode.empty() ? mnemonic : mnemonic + "_" + addrMode;
            allKeys.insert(key);

            if (!model_.hasEnergy(mnemonic, addrMode))
                missingKeys.insert(key);
        }
    }
}

} // namespace checkpoint
