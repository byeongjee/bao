#ifndef BASE_CONTEXT_H
#define BASE_CONTEXT_H

#include "common/CFGAnalysis.h"
#include "estimator/EnergyEstimator.h"
#include "estimator/EnergyEstimatorFactory.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"

#include <memory>
#include <string>

namespace checkpoint {

/// Common context shared by all checkpoint passes.
/// Encapsulates energy estimator, CFG analysis, loop info, and capacity.
struct BaseContext {
    std::unique_ptr<EnergyEstimator> estimator;
    std::unique_ptr<CFGAnalysis> cfg;
    llvm::LoopInfo *loopInfo;

    BaseContext(std::unique_ptr<EnergyEstimator> est, std::unique_ptr<CFGAnalysis> cfgAnalysis,
                llvm::LoopInfo *li)
        : estimator(std::move(est)), cfg(std::move(cfgAnalysis)), loopInfo(li) {}

    // Move-only
    BaseContext(BaseContext &&) = default;
    BaseContext &operator=(BaseContext &&) = default;
    BaseContext(const BaseContext &) = delete;
    BaseContext &operator=(const BaseContext &) = delete;

  protected:
    // Allow derived classes to default-construct for two-phase init
    BaseContext() : loopInfo(nullptr) {}
};

/// Generic result type for context creation.
/// On success, contains a context of type T.
/// On failure or skip, contains a status and optional error message.
template <typename T> struct ContextResult {
    enum class Status { Success, MissingConfig, EstimatorFailed, InvalidParams, IsDeclaration };

    Status status;
    std::unique_ptr<T> context;
    std::string errorMessage;

    bool success() const { return status == Status::Success; }
    bool shouldSkip() const { return status == Status::IsDeclaration; }

    static ContextResult ok(std::unique_ptr<T> ctx) {
        return {Status::Success, std::move(ctx), ""};
    }

    static ContextResult error(Status s, const std::string &msg) { return {s, nullptr, msg}; }

    static ContextResult skip() { return {Status::IsDeclaration, nullptr, ""}; }
};

/// Create a BaseContext from a function and config path.
/// Handles common validation: config check, estimator creation,
/// declaration skip, estimator prep, and CFG construction.
inline ContextResult<BaseContext> createBaseContext(llvm::Function &F, llvm::LoopInfo &LI,
                                                    llvm::StringRef configPath,
                                                    llvm::StringRef passName) {

    using Result = ContextResult<BaseContext>;

    // Validate required config
    if (configPath.empty()) {
        return Result::error(Result::Status::MissingConfig,
                             ("Error: config path is required for " + passName + "\n").str());
    }

    // Create energy estimator from config using default factory
    auto factory = EnergyEstimatorFactory::createDefault();
    auto estimator = factory.createFromConfig(configPath.str());
    if (!estimator) {
        return Result::error(Result::Status::EstimatorFailed,
                             "Failed to create energy estimator\n");
    }

    // Skip declarations
    if (F.isDeclaration()) {
        return Result::skip();
    }

    // Prepare estimator for this function (needed by AssemblyBasedEstimator)
    estimator->prepareForFunction(F);

    // Create CFG analysis
    auto cfg = std::make_unique<CFGAnalysis>(F, LI, *estimator);

    return Result::ok(std::make_unique<BaseContext>(std::move(estimator), std::move(cfg), &LI));
}

} // namespace checkpoint

#endif // BASE_CONTEXT_H
