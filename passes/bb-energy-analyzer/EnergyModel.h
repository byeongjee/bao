#pragma once

#include <string>
#include <unordered_map>

namespace bbanalyzer {

/// Energy model for MSP430 assembly instructions.
/// Loads costs from JSON configuration file.
class EnergyModel {
public:
    /// Load energy model from JSON config
    /// @param configPath Path to assembly energy config JSON
    explicit EnergyModel(const std::string &configPath);

    /// Get energy cost for an instruction
    /// @param mnemonic Instruction mnemonic (e.g., "mov")
    /// @param addrMode Addressing mode (e.g., "register_indexed")
    /// @return Energy cost in configured units (typically nJ)
    double getEnergy(const std::string &mnemonic,
                     const std::string &addrMode) const;

    /// Get default energy for unknown instructions
    double getDefaultEnergy() const { return defaultEnergy_; }

    /// Check if a specific instruction/mode combo is in the model
    bool hasEnergy(const std::string &mnemonic,
                   const std::string &addrMode) const;

private:
    // Key format: "mnemonic_addrmode" (e.g., "mov_register_indexed")
    std::unordered_map<std::string, double> costs_;
    double defaultEnergy_ = 1.0;

    /// Build lookup key from mnemonic and addressing mode
    static std::string makeKey(const std::string &mnemonic,
                               const std::string &addrMode);
};

} // namespace bbanalyzer
