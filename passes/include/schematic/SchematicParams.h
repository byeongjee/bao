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
    double loopIncrementCostNvm;    // Per-iteration energy of the conditional checkpoint
                                    // counter logic inserted by SCHEMATIC on loop back-edges:
                                    // load counter, compare against threshold, increment, store.
                                    // Differs by opt level (O0: memory-resident; O3: register).
    double callCost;                // Per-call overhead charged once at each function's entry
                                    // (reference self.call_cost, schematic.py:62,184-186).
    unsigned maxPaths;              // Maximum paths to enumerate
    bool addDebugMarkers = false;   // Emit debug marker calls
    bool forceCheckpointOnIncompatibleLoops = false; // Force checkpoint at loop header
                                                     // when inner loop allocation conflicts
                                                     // with previously fixed allocations
    bool recomputeEnergyAfterNewCheckpoint = false;  // Recompute local E_left/E_to_leave
                                                     // around new checkpoints. Disabled by
                                                     // default to preserve reference behavior.
};

/// Parse SCHEMATIC parameters from a JSON config file.
/// All fields required except add_debug_markers (optional, defaults false).
/// Returns std::nullopt on error.
std::optional<SchematicParams> parseSchematicParams(const std::string &configPath);

} // namespace checkpoint
