#pragma once

#include <string>
#include <unordered_map>

namespace checkpoint {

/// Singleton configuration loaded from JSON.
/// Must be initialized before any energy calculations.
class EnergyConfig {
public:
    /// Load configuration from JSON file.
    /// @param path Path to JSON configuration file.
    /// Calls llvm::report_fatal_error if file not found or invalid.
    static void loadFromFile(const std::string &path);

    /// Check if configuration has been loaded.
    static bool isLoaded();

    /// Get instruction cost by category name.
    /// Categories: simple_arithmetic, complex_arithmetic, floating_point,
    /// load, store, control_flow, comparison, conversion, call,
    /// phi_select, gep, alloca, atomic, default
    static int getInstructionCost(const std::string &category);

    /// Get energy capacity.
    static double getCapacity();

private:
    static bool loaded_;
    static double capacity_;
    static std::unordered_map<std::string, int> instructionCosts_;

    // Prevent instantiation
    EnergyConfig() = delete;
};

} // namespace checkpoint
