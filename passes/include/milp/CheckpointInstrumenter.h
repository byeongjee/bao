#pragma once

#include "milp/CheckpointOptimizer.h"
#include "milp/StateAnalysis.h"

#include "llvm/IR/Module.h"

#include <set>
#include <string>

namespace checkpoint {

/// Instruments LLVM IR based on MILP solution.
///
/// Handles:
/// - Region boundaries: prologue/epilogue calls
/// - Boundary commits/restores for candidate globals
/// - Candidate global section marking for runtime handling
class CheckpointInstrumenter {
public:
    CheckpointInstrumenter(llvm::Module &M);

    /// Instrument a function using the MILP solution and state analysis.
    /// @param F The function to instrument.
    /// @param solution The MILP solution.
    /// @param state The state analysis results.
    /// @return Number of instrumentation points inserted.
    unsigned instrumentFunction(llvm::Function &F,
                                const MILPSolution &solution,
                                const StateAnalysis &state);

private:
    llvm::Module &M_;

    // Runtime function callees (lazily declared)
    llvm::FunctionCallee prologueFn_;
    llvm::FunctionCallee epilogueFn_;
    llvm::FunctionCallee storeMemFn_;
    llvm::FunctionCallee restoreMemFn_;

    void declareRuntimeFunctions();

    unsigned insertRegionBoundaries(llvm::Function &F,
                                    const MILPSolution &solution,
                                    const StateAnalysis &state);
    void applyMemoryPlacement(const StateAnalysis &state);
};

} // namespace checkpoint
