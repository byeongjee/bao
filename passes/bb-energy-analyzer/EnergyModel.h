#pragma once

#include <optional>
#include <set>
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
    double getEnergy(const std::string &mnemonic, const std::string &addrMode) const;

    /// Get energy for a call instruction, using per-function key if target is known.
    /// For memcpy/memset with a known size, computes:
    ///   call_{func} + sizeArg * call_{func}_bytes
    /// Otherwise fallback chain: call_{function_name} -> call_{addrMode} -> default
    /// Returns 0.0 if callTarget is in ignored_call_targets whitelist.
    double getCallEnergy(const std::string &addrMode, const std::string &callTarget,
                         std::optional<unsigned> sizeArg = std::nullopt) const;

    /// Get default energy for unknown instructions
    double getDefaultEnergy() const { return defaultEnergy_; }

    /// Check if a specific instruction/mode combo is in the model
    bool hasEnergy(const std::string &mnemonic, const std::string &addrMode) const;

    /// Get all keys that have been queried via getEnergy()
    const std::set<std::string> &getRequiredKeys() const { return requiredKeys_; }

    /// Get keys that were queried but not found in the model
    const std::set<std::string> &getMissingKeys() const { return missingKeys_; }

  private:
    // Key format: "mnemonic_addrmode" (e.g., "mov_register_indexed")
    std::unordered_map<std::string, double> costs_;
    double defaultEnergy_ = 1.0;
    mutable std::set<std::string> requiredKeys_;
    mutable std::set<std::string> missingKeys_;
    std::set<std::string> ignoredCallTargets_;

    /// Build lookup key from mnemonic and addressing mode
    static std::string makeKey(const std::string &mnemonic, const std::string &addrMode);
};

} // namespace bbanalyzer
