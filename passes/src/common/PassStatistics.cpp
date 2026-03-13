#include "common/PassStatistics.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/JSON.h"

using namespace llvm;

cl::opt<std::string> StatsJsonOpt("ckpt-stats-json", cl::desc("Path to write pass statistics JSON"),
                                  cl::value_desc("filename"), cl::init(""));

namespace checkpoint {

void printCommonStats(llvm::raw_ostream &OS, const CommonStats &stats) {
    OS << "=== Checkpoint Insertion Statistics ===\n";
    OS << "  Pass:                            " << stats.passName << "\n";
    OS << "  Function:                        " << stats.functionName << "\n";
    OS << "  Basic blocks:                    " << stats.basicBlocks << "\n";
    OS << "  Edges:                           " << stats.edges << "\n";
    OS << "  Candidate globals (V_elig):      " << stats.candidateGlobals << "\n";
    OS << "  Regions:                         " << stats.regions << "\n";
    OS << "  Region boundaries:               " << stats.regionBoundaries << "\n";
    OS << "  Runtime calls inserted:          " << stats.runtimeCallsInserted << "\n";
    OS << "  Compilation time (ms):           " << llvm::format("%.3f", stats.compilationTimeMs)
       << "\n";
    OS << "  Peak RSS (KB):                   " << stats.peakRSSKb << "\n";
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
        llvm::errs() << "Warning: Cannot write stats JSON to " << path << ": " << EC.message()
                     << "\n";
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
