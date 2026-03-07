#include "common/PassStatistics.h"

#include "llvm/Support/Format.h"

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
    OS << "  Compilation time (ms):           "
       << llvm::format("%.3f", stats.compilationTimeMs) << "\n";
    OS << "  Peak RSS (KB):                   " << stats.peakRSSKb << "\n";
}

} // namespace checkpoint
