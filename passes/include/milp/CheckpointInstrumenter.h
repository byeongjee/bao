#pragma once

#include "milp/CheckpointOptimizer.h"
#include "milp/ModelViews.h"
#include "milp/StateAnalysis.h"

#include "llvm/IR/Module.h"

namespace checkpoint {

/// Instruments LLVM IR based on MILP solution.
class CheckpointInstrumenter {
public:
    explicit CheckpointInstrumenter(llvm::Module &M);

    /// Instrument a function using the MILP solution and state analysis.
    unsigned instrumentFunction(llvm::Function &F,
                                const MILPSolution &solution,
                                const ICFGView &cfg,
                                const StateAnalysis &state);

private:
    llvm::Module &M_;

    llvm::FunctionCallee prologueFn_;
    llvm::FunctionCallee epilogueFn_;
    llvm::FunctionCallee storeMemFn_;
    llvm::FunctionCallee restoreMemFn_;

    void declareRuntimeFunctions();

    unsigned insertRegionBoundaries(llvm::Function &F,
                                    const MILPSolution &solution,
                                    const ICFGView &cfg,
                                    const StateAnalysis &state);
    void applyMemoryPlacement(const StateAnalysis &state);
};

} // namespace checkpoint
