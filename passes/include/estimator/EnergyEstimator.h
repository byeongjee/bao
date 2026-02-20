#pragma once

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <memory>
#include <string>

namespace checkpoint {

/// Result of energy estimation for a basic block.
struct EnergyEstimate {
    double cost;         // Energy cost for the block
    std::string method;  // Estimation method used (for diagnostics)
};

/// Abstract interface for energy estimation.
/// Different implementations can provide energy estimates based on
/// IR analysis, assembly analysis, ML models, etc.
class EnergyEstimator {
public:
    virtual ~EnergyEstimator() = default;

    /// Core interface: estimate energy for a basic block.
    /// @param BB The basic block to estimate energy for.
    /// @return EnergyEstimate with cost and method description.
    virtual EnergyEstimate estimate(const llvm::BasicBlock &BB) = 0;

    /// Get estimator name for diagnostics.
    /// @return A human-readable name for this estimator.
    virtual std::string getName() const = 0;

    /// Optional hook called before processing a function.
    /// Implementations can override to perform function-level setup.
    /// @param F The function about to be processed.
    virtual void prepareForFunction(const llvm::Function &F) {}

    /// Optional hook called after processing a function.
    /// Implementations can override to perform function-level cleanup.
    /// @param F The function that was processed.
    virtual void finalizeFunction(const llvm::Function &F) {}

    /// Get the energy cost of a single instruction.
    /// Default returns 0 (subclasses should override for block splitting support).
    /// @param I The instruction to estimate energy for.
    /// @return Energy cost for the instruction.
    virtual double getInstructionCost(const llvm::Instruction &I) { return 0.0; }
};

using EnergyEstimatorPtr = std::unique_ptr<EnergyEstimator>;

} // namespace checkpoint
