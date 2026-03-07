#pragma once

#include "llvm/Support/raw_ostream.h"

#include <string>

namespace checkpoint {

/// Common statistics shared across all checkpoint insertion passes.
struct CommonStats {
    std::string passName;
    std::string functionName;
    unsigned basicBlocks = 0;
    unsigned edges = 0;
    unsigned candidateGlobals = 0;
    unsigned regions = 0;
    unsigned regionBoundaries = 0;
    unsigned runtimeCallsInserted = 0;
    double compilationTimeMs = 0.0;
    long peakRSSKb = 0;
};

/// Print the common statistics header block.
/// Each pass calls this, then prints its own pass-specific section.
void printCommonStats(llvm::raw_ostream &OS, const CommonStats &stats);

} // namespace checkpoint
