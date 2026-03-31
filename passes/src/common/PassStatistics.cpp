#include "common/PassStatistics.h"
#include "common/Logger.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"

using namespace llvm;

cl::opt<std::string> StatsJsonOpt("ckpt-stats-json", cl::desc("Path to write pass statistics JSON"),
                                  cl::value_desc("filename"), cl::init(""));

namespace checkpoint {

void printCommonStats(const CommonStats &stats) {
    PLOGI << "=== Checkpoint Insertion Statistics ===";
    PLOGI << "  Pass:                            " << stats.passName;
    PLOGI << "  Function:                        " << stats.functionName;
    PLOGI << "  Basic blocks:                    " << stats.basicBlocks;
    PLOGI << "  Edges:                           " << stats.edges;
    PLOGI << "  Candidate globals (V_elig):      " << stats.candidateGlobals;
    PLOGI << "  Regions:                         " << stats.regions;
    PLOGI << "  Region boundaries:               " << stats.regionBoundaries;
    PLOGI << "  Compilation time (ms):           "
          << checkpoint::fmtDouble(stats.compilationTimeMs);
    PLOGI << "  Peak RSS (KB):                   " << stats.peakRSSKb;
}

llvm::json::Object commonStatsToJSON(const CommonStats &stats) {
    llvm::json::Object obj;
    obj["pass"] = stats.passName;
    obj["function"] = stats.functionName;
    obj["basic_blocks"] = static_cast<int64_t>(stats.basicBlocks);
    obj["edges"] = static_cast<int64_t>(stats.edges);
    obj["candidate_globals"] = static_cast<int64_t>(stats.candidateGlobals);
    obj["regions"] = static_cast<int64_t>(stats.regions);
    obj["region_boundaries"] = static_cast<int64_t>(stats.regionBoundaries);
    obj["runtime_calls_inserted"] = static_cast<int64_t>(stats.runtimeCallsInserted);
    obj["compilation_time_ms"] = stats.compilationTimeMs;
    obj["peak_rss_kb"] = stats.peakRSSKb;
    return obj;
}

bool writeStatsJSON(llvm::StringRef path, llvm::json::Object root) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(path, EC);
    if (EC) {
        PLOGW << "Warning: Cannot write stats JSON to " << path.str() << ": " << EC.message();
        return false;
    }
    OS << llvm::json::Value(std::move(root));
    OS.flush();
    if (OS.has_error()) {
        OS.clear_error();
        return false;
    }
    return true;
}

} // namespace checkpoint
