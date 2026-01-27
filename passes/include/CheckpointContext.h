#ifndef CHECKPOINT_CONTEXT_H
#define CHECKPOINT_CONTEXT_H

#include "CFGAnalysis.h"
#include "EnergyEstimator.h"
#include "EnergyEstimatorFactory.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <string>

namespace checkpoint {

/// Common context for checkpoint passes.
/// Encapsulates energy estimator, CFG analysis, and capacity.
struct CheckpointContext {
    std::unique_ptr<EnergyEstimator> estimator;
    std::unique_ptr<CFGAnalysis> cfg;
    llvm::LoopInfo *loopInfo;
    double capacity;

    CheckpointContext(std::unique_ptr<EnergyEstimator> est,
                      std::unique_ptr<CFGAnalysis> cfgAnalysis,
                      llvm::LoopInfo *li,
                      double cap)
        : estimator(std::move(est)),
          cfg(std::move(cfgAnalysis)),
          loopInfo(li),
          capacity(cap) {}

    // Move-only
    CheckpointContext(CheckpointContext &&) = default;
    CheckpointContext &operator=(CheckpointContext &&) = default;
    CheckpointContext(const CheckpointContext &) = delete;
    CheckpointContext &operator=(const CheckpointContext &) = delete;
};

/// Result type for context creation.
/// On success, contains CheckpointContext.
/// On failure, contains an error message.
struct ContextResult {
    enum class Status {
        Success,
        MissingConfig,
        EstimatorFailed,
        IsDeclaration
    };

    Status status;
    std::unique_ptr<CheckpointContext> context;
    std::string errorMessage;

    bool success() const { return status == Status::Success; }
    bool shouldSkip() const { return status == Status::IsDeclaration; }

    static ContextResult ok(std::unique_ptr<CheckpointContext> ctx) {
        return {Status::Success, std::move(ctx), ""};
    }

    static ContextResult error(Status s, const std::string &msg) {
        return {s, nullptr, msg};
    }

    static ContextResult skip() {
        return {Status::IsDeclaration, nullptr, ""};
    }
};

/// Create checkpoint context from function and config path.
/// Returns ContextResult indicating success, error, or skip.
inline ContextResult createCheckpointContext(
    llvm::Function &F,
    llvm::LoopInfo &LI,
    llvm::StringRef configPath,
    llvm::StringRef passName) {

    // Validate required config
    if (configPath.empty()) {
        return ContextResult::error(
            ContextResult::Status::MissingConfig,
            ("Error: -energy-config is required for " + passName + "\n").str());
    }

    // Create energy estimator from config using default factory
    auto factory = EnergyEstimatorFactory::createDefault();
    auto estimator = factory.createFromConfig(configPath.str());
    if (!estimator) {
        return ContextResult::error(
            ContextResult::Status::EstimatorFailed,
            "Failed to create energy estimator\n");
    }

    // Skip declarations
    if (F.isDeclaration()) {
        return ContextResult::skip();
    }

    double capacity = estimator->getCapacity();

    // Create CFG analysis
    auto cfg = std::make_unique<CFGAnalysis>(F, LI, *estimator);

    return ContextResult::ok(std::make_unique<CheckpointContext>(
        std::move(estimator), std::move(cfg), &LI, capacity));
}

} // namespace checkpoint

#endif // CHECKPOINT_CONTEXT_H
