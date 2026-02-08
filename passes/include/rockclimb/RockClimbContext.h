#ifndef ROCKCLIMB_CONTEXT_H
#define ROCKCLIMB_CONTEXT_H

#include "common/BaseContext.h"

#include "llvm/Support/raw_ostream.h"

namespace checkpoint {

/// RockClimb-specific parameters from config.
struct RockClimbParams {
    double V_max = 3.6;           // Maximum voltage (V)
    double V_min = 1.8;           // Minimum operating voltage (V)
    double C_buf_uF = 10.0;       // Buffer capacitance (microfarads)
    unsigned N_reg = 16;          // Number of registers
    double E_restore_per_reg = 0.5;  // Energy to restore one register
    bool distributedCheckpointing = true;  // Enable distributed register checkpointing
    bool memoryCheckpointing = false;      // Enable memory (alloca/global) checkpointing

    /// Calculate E_safe from physical parameters.
    /// E_safe = E_input - E_restore
    /// E_input = 0.5 * C_buf * (V_max^2 - V_min^2)
    /// E_restore = N_reg * E_restore_per_reg
    double calculateESafe() const {
        double C_buf_F = C_buf_uF * 1e-6;  // Convert to Farads
        double E_input = 0.5 * C_buf_F * (V_max * V_max - V_min * V_min);
        double E_restore = N_reg * E_restore_per_reg;
        // Convert to same units as energy model (multiply by 1e6 for micro-Joules scale)
        return E_input * 1e6 - E_restore;
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
                     double cap,
                     const RockClimbParams &p)
        : BaseContext(std::move(est), std::move(cfgAnalysis), li, cap),
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

/// Create RockClimb context from function and config path.
inline RockClimbContextResult createRockClimbContext(
    llvm::Function &F,
    llvm::LoopInfo &LI,
    llvm::StringRef configPath) {

    // Use createBaseContext for common setup
    auto baseResult = createBaseContext(F, LI, configPath, "rockclimb pass");

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

    // Skip generated restore functions (from memory checkpointing)
    if (F.getName().starts_with("__restore_boundary_")) {
        return RockClimbContextResult::skip();
    }

    // Parse RockClimb-specific parameters
    RockClimbParams params;
    if (!parseRockClimbParams(configPath, params)) {
        // Use defaults if no rockclimb_parameters section
        llvm::errs() << "Warning: No rockclimb_parameters in config, using defaults\n";
    }

    return RockClimbContextResult::ok(std::make_unique<RockClimbContext>(
        std::move(base.estimator), std::move(base.cfg),
        base.loopInfo, base.capacity, params));
}

} // namespace checkpoint

#endif // ROCKCLIMB_CONTEXT_H
