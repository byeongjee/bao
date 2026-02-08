#include "MILPContext.h"

namespace checkpoint {

MILPContextResult createMILPContext(llvm::Function &F,
                                    llvm::LoopInfo &LI,
                                    llvm::StringRef energyConfigPath,
                                    llvm::StringRef milpConfigPath,
                                    llvm::StringRef passName) {
    if (energyConfigPath.empty()) {
        return MILPContextResult::error(
            MILPContextResult::Status::MissingEnergyConfig,
            ("Error: -energy-config is required for " + passName + "\n").str());
    }
    if (milpConfigPath.empty()) {
        return MILPContextResult::error(
            MILPContextResult::Status::MissingMILPConfig,
            ("Error: -milp-config is required for " + passName + "\n").str());
    }
    if (F.isDeclaration()) {
        return MILPContextResult::skip();
    }

    auto factory = EnergyEstimatorFactory::createDefault();
    auto estimator = factory.createFromConfig(energyConfigPath.str());
    if (!estimator) {
        return MILPContextResult::error(
            MILPContextResult::Status::EstimatorFailed,
            "Failed to create energy estimator\n");
    }

    auto parseResult = parseMILPParametersFromFile(milpConfigPath);
    if (!parseResult.success()) {
        return MILPContextResult::error(
            MILPContextResult::Status::MILPConfigFailed,
            parseResult.errorMessage + "\n");
    }

    estimator->prepareForFunction(F);
    auto cfg = std::make_unique<CFGAnalysis>(F, LI, *estimator);

    return MILPContextResult::ok(std::make_unique<MILPContext>(
        std::move(estimator), std::move(cfg), &LI, std::move(parseResult.parameters)));
}

} // namespace checkpoint
