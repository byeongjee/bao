#ifndef ROCKCLIMB_CONTEXT_H
#define ROCKCLIMB_CONTEXT_H

#include "common/BaseContext.h"

#include "llvm/Support/raw_ostream.h"

namespace checkpoint {

/// RockClimb-specific parameters from config.
struct RockClimbParams {
    double E_input;                // Harvestable energy per cycle (same units as energy model)
    unsigned N_reg;                // Number of registers
    double E_restore_per_reg;      // Energy to restore one register
    bool distributedCheckpointing; // Enable distributed register checkpointing
    bool addDebugMarkers = false;  // Emit debug marker calls for runtime counters
    double checkpoint_store_energy = 0.0; // Energy cost per checkpoint store (for CkptCycles)

    /// Calculate E_safe = E_input - E_restore.
    double calculateESafe() const {
        return E_input - N_reg * E_restore_per_reg;
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
