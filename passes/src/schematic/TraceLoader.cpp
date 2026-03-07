#include "schematic/TraceLoader.h"

#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <nlohmann/json.hpp>

using namespace llvm;
using json = nlohmann::json;

namespace checkpoint {

TraceLoader::TraceLoader(Function &F, LoopInfo &LI) : F_(F), LI_(LI) {
    // Build name -> BB* map
    for (BasicBlock &BB : F_) {
        if (BB.hasName())
            nameToBlock_[BB.getName()] = &BB;
    }
}

std::optional<LoadedTraces> TraceLoader::load(const std::string &traceFilePath) {
    // Read and parse JSON
    std::ifstream ifs(traceFilePath);
    if (!ifs.is_open()) {
        errs() << "TraceLoader: cannot open " << traceFilePath << "\n";
        return std::nullopt;
    }

    json root = json::parse(ifs, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        errs() << "TraceLoader: JSON parse error in " << traceFilePath << "\n";
        return std::nullopt;
    }

    // Look up function name
    std::string funcName = F_.getName().str();
    if (!root.contains(funcName)) {
        errs() << "TraceLoader: function '" << funcName << "' not found in trace file\n";
        return std::nullopt;
    }

    const json &funcObj = root[funcName];
    LoadedTraces result;

    // -----------------------------------------------------------------------
    // Parse function traces
    // -----------------------------------------------------------------------
    if (funcObj.contains("traces") && funcObj["traces"].is_array()) {
        for (const auto &traceObj : funcObj["traces"]) {
            if (!traceObj.contains("path") || !traceObj.contains("count"))
                continue;

            EnumeratedPath ep;
            bool valid = true;
            for (const auto &bbName : traceObj["path"]) {
                std::string name = bbName.get<std::string>();
                auto it = nameToBlock_.find(StringRef(name));
                if (it == nameToBlock_.end()) {
                    errs() << "TraceLoader: warning: BB '" << name << "' not found in " << funcName
                           << ", skipping trace\n";
                    valid = false;
                    break;
                }
                ep.blocks.push_back(it->second);
            }
            if (!valid)
                continue;

            unsigned count = traceObj["count"].get<unsigned>();
            ep.count = count;
            ep.probability = static_cast<double>(count);
            result.functionPaths.push_back(std::move(ep));
        }

        // Sort by decreasing count
        std::sort(
            result.functionPaths.begin(), result.functionPaths.end(),
            [](const EnumeratedPath &a, const EnumeratedPath &b) { return a.count > b.count; });
    }

    // -----------------------------------------------------------------------
    // Parse loop traces
    // -----------------------------------------------------------------------
    if (funcObj.contains("loop_traces") && funcObj["loop_traces"].is_object()) {
        for (auto &[headerName, loopObj] : funcObj["loop_traces"].items()) {
            // Resolve header BB
            auto headerIt = nameToBlock_.find(StringRef(headerName));
            if (headerIt == nameToBlock_.end()) {
                errs() << "TraceLoader: warning: loop header '" << headerName
                       << "' not found, skipping loop\n";
                continue;
            }

            LoadedLoopTrace llt;
            llt.header = headerIt->second;
            llt.loop = nullptr;
            llt.latch = nullptr;
            llt.depth = 0;

            // Find matching LLVM Loop*
            for (Loop *L : LI_.getLoopsInPreorder()) {
                if (L->getHeader() == llt.header) {
                    llt.loop = L;
                    break;
                }
            }

            if (!llt.loop) {
                errs() << "TraceLoader: warning: no Loop* for header '" << headerName
                       << "', skipping\n";
                continue;
            }

            // Parse loop metadata
            if (loopObj.contains("loop") && loopObj["loop"].is_object()) {
                const json &loopMeta = loopObj["loop"];

                // Latch
                if (loopMeta.contains("latch") && loopMeta["latch"].is_array()) {
                    for (const auto &ln : loopMeta["latch"]) {
                        std::string name = ln.get<std::string>();
                        auto it = nameToBlock_.find(StringRef(name));
                        if (it != nameToBlock_.end()) {
                            llt.latch = it->second;
                            break; // use first latch
                        }
                    }
                }

                // Members
                if (loopMeta.contains("basic_blocks") && loopMeta["basic_blocks"].is_array()) {
                    for (const auto &mn : loopMeta["basic_blocks"]) {
                        std::string name = mn.get<std::string>();
                        auto it = nameToBlock_.find(StringRef(name));
                        if (it != nameToBlock_.end())
                            llt.members.push_back(it->second);
                    }
                }

                // Depth
                if (loopMeta.contains("depth"))
                    llt.depth = loopMeta["depth"].get<unsigned>();
            }

            // Fallback latch/depth from LLVM LoopInfo
            if (!llt.latch)
                llt.latch = llt.loop->getLoopLatch();
            if (llt.depth == 0)
                llt.depth = llt.loop->getLoopDepth();

            // Parse iteration traces
            if (loopObj.contains("traces") && loopObj["traces"].is_array()) {
                for (const auto &traceObj : loopObj["traces"]) {
                    if (!traceObj.contains("path") || !traceObj.contains("count"))
                        continue;

                    EnumeratedPath ep;
                    bool valid = true;
                    for (const auto &bbName : traceObj["path"]) {
                        std::string name = bbName.get<std::string>();
                        auto it = nameToBlock_.find(StringRef(name));
                        if (it == nameToBlock_.end()) {
                            errs() << "TraceLoader: warning: BB '" << name
                                   << "' in loop trace not found, skipping\n";
                            valid = false;
                            break;
                        }
                        ep.blocks.push_back(it->second);
                    }
                    if (!valid)
                        continue;

                    unsigned count = traceObj["count"].get<unsigned>();
                    ep.count = count;
                    ep.probability = static_cast<double>(count);
                    llt.iterationPaths.push_back(std::move(ep));
                }

                // Sort by decreasing count
                std::sort(llt.iterationPaths.begin(), llt.iterationPaths.end(),
                          [](const EnumeratedPath &a, const EnumeratedPath &b) {
                              return a.count > b.count;
                          });
            }

            result.loopTraces.push_back(std::move(llt));
        }
    }

    errs() << "TraceLoader: loaded " << result.functionPaths.size() << " function traces, "
           << result.loopTraces.size() << " loop traces for " << funcName << "\n";

    return result;
}

} // namespace checkpoint
