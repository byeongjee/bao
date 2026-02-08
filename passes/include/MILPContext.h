#pragma once

#include "CFGAnalysis.h"
#include "EnergyEstimator.h"
#include "EnergyEstimatorFactory.h"
#include "MILPParameters.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"

#include <memory>
#include <string>

namespace checkpoint {

struct MILPContext {
    std::unique_ptr<EnergyEstimator> estimator;
    std::unique_ptr<CFGAnalysis> cfg;
    llvm::LoopInfo *loopInfo = nullptr;
    MILPParameters milpParameters;

    MILPContext(std::unique_ptr<EnergyEstimator> est,
                std::unique_ptr<CFGAnalysis> cfgAnalysis,
                llvm::LoopInfo *li,
                MILPParameters params)
        : estimator(std::move(est)),
          cfg(std::move(cfgAnalysis)),
          loopInfo(li),
          milpParameters(std::move(params)) {}

    MILPContext(MILPContext &&) = default;
    MILPContext &operator=(MILPContext &&) = default;
    MILPContext(const MILPContext &) = delete;
    MILPContext &operator=(const MILPContext &) = delete;
};

struct MILPContextResult {
    enum class Status {
        Success,
        MissingEnergyConfig,
        MissingMILPConfig,
        EstimatorFailed,
        MILPConfigFailed,
        IsDeclaration
    };

    Status status = Status::Success;
    std::unique_ptr<MILPContext> context;
    std::string errorMessage;

    bool success() const { return status == Status::Success; }
    bool shouldSkip() const { return status == Status::IsDeclaration; }

    static MILPContextResult ok(std::unique_ptr<MILPContext> ctx) {
        MILPContextResult result;
        result.status = Status::Success;
        result.context = std::move(ctx);
        return result;
    }

    static MILPContextResult error(Status st, std::string message) {
        MILPContextResult result;
        result.status = st;
        result.errorMessage = std::move(message);
        return result;
    }

    static MILPContextResult skip() {
        MILPContextResult result;
        result.status = Status::IsDeclaration;
        return result;
    }
};

MILPContextResult createMILPContext(llvm::Function &F,
                                    llvm::LoopInfo &LI,
                                    llvm::StringRef energyConfigPath,
                                    llvm::StringRef milpConfigPath,
                                    llvm::StringRef passName);

} // namespace checkpoint
