#pragma once

#include "llvm/IR/PassManager.h"

namespace checkpoint {

/// LLVM pass for dynamic energy validation.
/// Instruments IR to track energy consumption at runtime and detect violations.
/// Works with both MILP and RockClimb checkpoint insertion modes.
class EnergyValidatorPass : public llvm::PassInfoMixin<EnergyValidatorPass> {
  public:
    /// Run the pass on a function.
    llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);

    /// Pass name for registration.
    static llvm::StringRef name() { return "EnergyValidatorPass"; }
};

} // namespace checkpoint
