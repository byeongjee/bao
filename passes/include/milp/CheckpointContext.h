#ifndef CHECKPOINT_CONTEXT_H
#define CHECKPOINT_CONTEXT_H

#include "common/BaseContext.h"
#include "milp/EnergyModel.h"
#include "milp/StateAnalysis.h"

#include <memory>

namespace checkpoint {

/// MILP checkpoint context extends BaseContext with state analysis and energy
/// model.
struct CheckpointContext : public BaseContext {
    std::unique_ptr<StateAnalysis> stateAnalysis;
    std::unique_ptr<EnergyModel> energyModel;
    MILPEnergyParams milpParams;

    CheckpointContext(std::unique_ptr<EnergyEstimator> est,
                      std::unique_ptr<CFGAnalysis> cfgAnalysis,
                      llvm::LoopInfo *li,
                      double cap)
        : BaseContext(std::move(est), std::move(cfgAnalysis), li, cap) {}

    // Move-only
    CheckpointContext(CheckpointContext &&) = default;
    CheckpointContext &operator=(CheckpointContext &&) = default;
};

/// Result type for checkpoint context creation.
using CheckpointContextResult = ContextResult<CheckpointContext>;

/// Create a basic checkpoint context from function and config path.
/// This creates the base context (estimator + CFG) without the new analyses.
/// The caller is responsible for adding StateAnalysis and EnergyModel.
inline CheckpointContextResult createCheckpointContext(
    llvm::Function &F,
    llvm::LoopInfo &LI,
    llvm::StringRef configPath,
    llvm::StringRef passName) {

    using Result = ContextResult<CheckpointContext>;

    // Validate required config
    if (configPath.empty()) {
        return Result::error(
            Result::Status::MissingConfig,
            ("Error: config path is required for " + passName + "\n").str());
    }

    // Create energy estimator from config using default factory
    auto factory = EnergyEstimatorFactory::createDefault();
    auto estimator = factory.createFromConfig(configPath.str());
    if (!estimator) {
        return Result::error(
            Result::Status::EstimatorFailed,
            "Failed to create energy estimator\n");
    }

    // Skip declarations
    if (F.isDeclaration()) {
        return Result::skip();
    }

    double capacity = estimator->getCapacity();

    // Prepare estimator for this function
    estimator->prepareForFunction(F);

    // Create CFG analysis
    auto cfg = std::make_unique<CFGAnalysis>(F, LI, *estimator);

    return Result::ok(std::make_unique<CheckpointContext>(
        std::move(estimator), std::move(cfg), &LI, capacity));
}

} // namespace checkpoint

#endif // CHECKPOINT_CONTEXT_H
