#ifndef ROCKCLIMB_CONTEXT_H
#define ROCKCLIMB_CONTEXT_H

#include "CFGAnalysis.h"
#include "EnergyEstimator.h"
#include "EnergyEstimatorFactory.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>
#include <string>

namespace checkpoint {

/// RockClimb-specific parameters from config.
struct RockClimbParams {
    double V_max = 3.6;           // Maximum voltage (V)
    double V_min = 1.8;           // Minimum operating voltage (V)
    double C_buf_uF = 10.0;       // Buffer capacitance (microfarads)
    unsigned N_reg = 16;          // Number of registers
    double E_restore_per_reg = 0.5;  // Energy to restore one register
    bool distributedCheckpointing = true;  // Enable distributed checkpointing

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
/// Encapsulates energy estimator, CFG analysis, and RockClimb parameters.
struct RockClimbContext {
    std::unique_ptr<EnergyEstimator> estimator;
    std::unique_ptr<CFGAnalysis> cfg;
    llvm::LoopInfo *loopInfo;
    double capacity;           // From energy config (same as MILP)
    RockClimbParams params;    // RockClimb-specific parameters
    double E_safe;             // Calculated safe energy

    RockClimbContext(std::unique_ptr<EnergyEstimator> est,
                     std::unique_ptr<CFGAnalysis> cfgAnalysis,
                     llvm::LoopInfo *li,
                     double cap,
                     const RockClimbParams &p)
        : estimator(std::move(est)),
          cfg(std::move(cfgAnalysis)),
          loopInfo(li),
          capacity(cap),
          params(p),
          E_safe(p.calculateESafe()) {}

    // Move-only
    RockClimbContext(RockClimbContext &&) = default;
    RockClimbContext &operator=(RockClimbContext &&) = default;
    RockClimbContext(const RockClimbContext &) = delete;
    RockClimbContext &operator=(const RockClimbContext &) = delete;
};

/// Result type for RockClimb context creation.
struct RockClimbContextResult {
    enum class Status {
        Success,
        MissingConfig,
        EstimatorFailed,
        InvalidParams,
        IsDeclaration
    };

    Status status;
    std::unique_ptr<RockClimbContext> context;
    std::string errorMessage;

    bool success() const { return status == Status::Success; }
    bool shouldSkip() const { return status == Status::IsDeclaration; }

    static RockClimbContextResult ok(std::unique_ptr<RockClimbContext> ctx) {
        return {Status::Success, std::move(ctx), ""};
    }

    static RockClimbContextResult error(Status s, const std::string &msg) {
        return {s, nullptr, msg};
    }

    static RockClimbContextResult skip() {
        return {Status::IsDeclaration, nullptr, ""};
    }
};

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

    // Validate required config
    if (configPath.empty()) {
        return RockClimbContextResult::error(
            RockClimbContextResult::Status::MissingConfig,
            "Error: -rockclimb-config is required for rockclimb pass\n");
    }

    // Create energy estimator from config using default factory
    auto factory = EnergyEstimatorFactory::createDefault();
    auto estimator = factory.createFromConfig(configPath.str());
    if (!estimator) {
        return RockClimbContextResult::error(
            RockClimbContextResult::Status::EstimatorFailed,
            "Failed to create energy estimator\n");
    }

    // Skip declarations
    if (F.isDeclaration()) {
        return RockClimbContextResult::skip();
    }

    // Parse RockClimb-specific parameters
    RockClimbParams params;
    if (!parseRockClimbParams(configPath, params)) {
        // Use defaults if no rockclimb_parameters section
        // This allows using existing configs with default RockClimb params
        llvm::errs() << "Warning: No rockclimb_parameters in config, using defaults\n";
    }

    double capacity = estimator->getCapacity();

    // Prepare estimator for this function
    estimator->prepareForFunction(F);

    // Create CFG analysis
    auto cfg = std::make_unique<CFGAnalysis>(F, LI, *estimator);

    return RockClimbContextResult::ok(std::make_unique<RockClimbContext>(
        std::move(estimator), std::move(cfg), &LI, capacity, params));
}

} // namespace checkpoint

#endif // ROCKCLIMB_CONTEXT_H
