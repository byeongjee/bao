#pragma once

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"

#include <string>

/// Shared CLI option for writing pass statistics to a JSON sidecar file.
/// Defined in PassStatistics.cpp so it is available in every pass library.
extern llvm::cl::opt<std::string> StatsJsonOpt;

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
void printCommonStats(const CommonStats &stats);

/// Serialize common statistics to a JSON object.
llvm::json::Object commonStatsToJSON(const CommonStats &stats);

/// Write a JSON object to a file.  Returns true on success.
bool writeStatsJSON(llvm::StringRef path, llvm::json::Object root);

} // namespace checkpoint
