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
/// - Distributed checkpoint stores at definition sites
/// - Memory placement: setting globals to NVM section
/// - Restore calls at region prologues
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
    llvm::FunctionCallee storeRegFn_;
    llvm::FunctionCallee storeMemFn_;
    llvm::FunctionCallee restoreRegFn_;
    llvm::FunctionCallee restoreMemFn_;

    void declareRuntimeFunctions();

    void insertRegionBoundaries(llvm::Function &F,
                                const MILPSolution &solution,
                                const StateAnalysis &state);
    void insertDistributedStores(llvm::Function &F,
                                 const MILPSolution &solution,
                                 const StateAnalysis &state);
    void applyMemoryPlacement(const MILPSolution &solution);
};

} // namespace checkpoint
