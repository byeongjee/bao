#pragma once

#include <optional>
#include <string>

namespace checkpoint {

/// Parameters for the SCHEMATIC greedy heuristic checkpoint insertion.
struct SchematicParams {
    double capacity;                // E_buf: energy buffer capacity
    double E_pro;                   // Prologue energy at region boundary
    double E_epi;                   // Epilogue energy at region boundary
    unsigned N_reg;                 // Number of registers to checkpoint
    double regStoreEnergy;          // Energy to store one register to FRAM
    double regRestoreEnergy;        // Energy to restore one register from FRAM
    double nvmAccessPenalty;        // Extra energy per NVM access vs VM
    double memStoreEnergyPerByte;   // Energy per byte for VM->FRAM copy
    double memRestoreEnergyPerByte; // Energy per byte for FRAM->VM copy
    unsigned vmCapacityBytes;       // VM (SRAM) capacity in bytes
    unsigned maxPaths;              // Maximum paths to enumerate
    bool addDebugMarkers = false;   // Emit debug marker calls
};

/// Parse SCHEMATIC parameters from a JSON config file.
/// All fields required except add_debug_markers (optional, defaults false).
/// Returns std::nullopt on error.
std::optional<SchematicParams> parseSchematicParams(const std::string &configPath);

} // namespace checkpoint
