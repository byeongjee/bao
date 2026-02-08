#pragma once

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Function.h"

#include <map>

namespace checkpoint {

/// Extracts loop trip count bounds from __loop_tripcount() marker calls.
class LoopTripCount {
public:
    /// Extract trip counts from __loop_tripcount() calls in a function.
    /// Scans for calls to __loop_tripcount(N) and uses LoopInfo to determine
    /// which loop contains each call.
    ///
    /// @param F The function to analyze.
    /// @param LI Loop information for the function.
    /// @return Map from Loop* to the annotated trip count.
    static std::map<const llvm::Loop*, unsigned>
    extractBounds(llvm::Function &F, llvm::LoopInfo &LI);
};

} // namespace checkpoint
