#ifndef ROCKCLIMB_CONTEXT_H
#define ROCKCLIMB_CONTEXT_H

#include "common/BaseContext.h"

#include "llvm/Support/raw_ostream.h"

namespace checkpoint {

/// RockClimb-specific parameters from config.
struct RockClimbParams {
    double capacity;               // Energy buffer capacity (was E_input)
    double E_pro = 0.0;            // Prologue energy (shared field, unused by algorithm)
    double E_epi = 0.0;            // Epilogue energy (shared field, unused by algorithm)
    unsigned N_reg;                // Number of registers
    double reg_store_energy = 0.0; // Energy to store one register (shared field)
    double reg_restore_energy;      // Energy to restore one register
    double nvmAccessPenalty = 0.0; // NVM access penalty (shared field, unused by algorithm)
    double memStoreEnergyPerByte = 0.0;   // Energy per byte store (shared field)
    double memRestoreEnergyPerByte = 0.0; // Energy per byte restore (shared field)
    unsigned vmCapacityBytes = 0;  // VM capacity (shared field, unused by algorithm)
    bool distributedCheckpointing; // Enable distributed register checkpointing
    bool addDebugMarkers = false;  // Emit debug marker calls for runtime counters
    double checkpoint_store_energy = 0.0; // Energy cost per checkpoint store (for CkptCycles)

    /// Calculate E_safe = capacity - E_restore.
    double calculateESafe() const {
        return capacity - N_reg * reg_restore_energy;
    }
};

/// Context for RockClimb pass.
/// Extends BaseContext with RockClimb-specific parameters.
struct RockClimbContext : public BaseContext {
    RockClimbParams params;    // RockClimb-specific parameters
    double E_safe;             // Calculated safe energy

    RockClimbContext(std::unique_ptr<EnergyEstimator> est,
                     std::unique_ptr<CFGAnalysis> cfgAnalysis,
                     llvm::LoopInfo *li,
                     const RockClimbParams &p)
        : BaseContext(std::move(est), std::move(cfgAnalysis), li),
          params(p),
          E_safe(p.calculateESafe()) {}

    // Move-only
    RockClimbContext(RockClimbContext &&) = default;
    RockClimbContext &operator=(RockClimbContext &&) = default;
    RockClimbContext(const RockClimbContext &) = delete;
    RockClimbContext &operator=(const RockClimbContext &) = delete;
};

/// Result type for RockClimb context creation.
using RockClimbContextResult = ContextResult<RockClimbContext>;

/// Parse RockClimb parameters from JSON config file.
/// @param configPath Path to JSON config file.
/// @param params Output parameter struct.
/// @return True if parsing succeeded.
bool parseRockClimbParams(llvm::StringRef configPath, RockClimbParams &params);

/// Create RockClimb context from function and config paths.
/// @param estimatorConfigPath Path to energy estimator config JSON.
/// @param rockclimbConfigPath Path to RockClimb params config JSON.
inline RockClimbContextResult createRockClimbContext(
    llvm::Function &F,
    llvm::LoopInfo &LI,
    llvm::StringRef estimatorConfigPath,
    llvm::StringRef rockclimbConfigPath) {

    // Use createBaseContext for common setup (estimator config)
    auto baseResult = createBaseContext(F, LI, estimatorConfigPath, "rockclimb pass");

    if (!baseResult.success()) {
        // Propagate error/skip with matching status
        if (baseResult.shouldSkip()) {
            return RockClimbContextResult::skip();
        }
        // Map BaseContext status to RockClimbContext status
        RockClimbContextResult::Status s;
        switch (baseResult.status) {
        case ContextResult<BaseContext>::Status::MissingConfig:
            s = RockClimbContextResult::Status::MissingConfig;
            break;
        case ContextResult<BaseContext>::Status::EstimatorFailed:
            s = RockClimbContextResult::Status::EstimatorFailed;
            break;
        default:
            s = RockClimbContextResult::Status::EstimatorFailed;
            break;
        }
        return RockClimbContextResult::error(s, baseResult.errorMessage);
    }

    auto &base = *baseResult.context;

    // Parse RockClimb-specific parameters
    RockClimbParams params;
    if (!parseRockClimbParams(rockclimbConfigPath, params)) {
        return RockClimbContextResult::error(
            RockClimbContextResult::Status::InvalidParams,
            "Error: Failed to parse RockClimb config: " + rockclimbConfigPath.str() + "\n");
    }

    return RockClimbContextResult::ok(std::make_unique<RockClimbContext>(
        std::move(base.estimator), std::move(base.cfg),
        base.loopInfo, params));
}

} // namespace checkpoint

#endif // ROCKCLIMB_CONTEXT_H
