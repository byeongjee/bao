#include "schematic/TraceLoader.h"

#include "common/Logger.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>
#include <queue>

using namespace llvm;
using json = nlohmann::json;

namespace checkpoint {

namespace {

static LoadedLoopTrace makeCanonicalLoopTrace(Loop *L, SchematicGraph &graph) {
    LoadedLoopTrace llt;
    llt.loop = L;
    llt.header = graph.getOrCreate(L->getHeader());
    if (BasicBlock *latch = L->getLoopLatch())
        llt.latch = graph.getOrCreate(latch);
    else
        llt.latch = nullptr;
    llt.depth = L->getLoopDepth();
    for (BasicBlock *BB : L->blocks())
        llt.members.push_back(graph.getOrCreate(BB));
    return llt;
}

static SmallPtrSet<BasicBlock *, 16> collectLoopMemberSet(Loop *L) {
    SmallPtrSet<BasicBlock *, 16> members;
    for (BasicBlock *BB : L->blocks())
        members.insert(BB);
    return members;
}

static bool pathContainsOnlyLoopMembers(const EnumeratedPath &path,
                                        const SmallPtrSetImpl<BasicBlock *> &memberSet) {
    for (SchematicBlock *block : path.blocks) {
        BasicBlock *BB = block ? block->getLLVMBlock() : nullptr;
        if (!BB || !memberSet.contains(BB))
            return false;
    }
    return true;
}

static bool isStructurallyValidLoopPath(const EnumeratedPath &path, const LoadedLoopTrace &trace,
                                        const SmallPtrSetImpl<BasicBlock *> &memberSet) {
    if (path.blocks.empty() || !trace.header || !trace.latch)
        return false;
    if (path.blocks.front() != trace.header || path.blocks.back() != trace.latch)
        return false;
    return pathContainsOnlyLoopMembers(path, memberSet);
}

static bool pathsEqual(const EnumeratedPath &lhs, const EnumeratedPath &rhs) {
    if (lhs.blocks.size() != rhs.blocks.size())
        return false;
    for (size_t i = 0; i < lhs.blocks.size(); ++i) {
        if (lhs.blocks[i] != rhs.blocks[i])
            return false;
    }
    return true;
}

static bool pathAlreadyPresent(const std::vector<EnumeratedPath> &paths,
                               const EnumeratedPath &path) {
    return std::any_of(paths.begin(), paths.end(),
                       [&](const EnumeratedPath &existing) { return pathsEqual(existing, path); });
}

static Loop *getImmediateChildLoop(Loop *parent, BasicBlock *BB, LoopInfo &LI) {
    Loop *owner = LI.getLoopFor(BB);
    while (owner && owner != parent && owner->getParentLoop() != parent)
        owner = owner->getParentLoop();
    if (!owner || owner == parent)
        return nullptr;
    return owner;
}

static SmallVector<BasicBlock *, 4> getReducedSuccessors(Loop *L, BasicBlock *BB, LoopInfo &LI) {
    SmallVector<BasicBlock *, 4> successors;
    if (!BB)
        return successors;

    if (Loop *child = getImmediateChildLoop(L, BB, LI)) {
        if (BB != child->getHeader())
            return successors;

        SmallVector<BasicBlock *, 4> exitBlocks;
        child->getExitBlocks(exitBlocks);
        SmallPtrSet<BasicBlock *, 4> seen;
        for (BasicBlock *exitBB : exitBlocks) {
            if (!exitBB || !L->contains(exitBB))
                continue;
            if (seen.insert(exitBB).second)
                successors.push_back(exitBB);
        }
        return successors;
    }

    BasicBlock *header = L->getHeader();
    BasicBlock *latch = L->getLoopLatch();
    for (BasicBlock *succ : llvm::successors(BB)) {
        if (!L->contains(succ))
            continue;
        if (BB == latch && succ == header)
            continue;
        successors.push_back(succ);
    }
    return successors;
}

static std::optional<std::vector<BasicBlock *>> findReducedPath(Loop *L, BasicBlock *src,
                                                                BasicBlock *dst, LoopInfo &LI) {
    if (!src || !dst)
        return std::nullopt;
    if (src == dst)
        return std::vector<BasicBlock *>{src};

    std::queue<BasicBlock *> worklist;
    SmallPtrSet<BasicBlock *, 32> visited;
    DenseMap<BasicBlock *, BasicBlock *> predecessor;

    worklist.push(src);
    visited.insert(src);

    while (!worklist.empty()) {
        BasicBlock *current = worklist.front();
        worklist.pop();

        for (BasicBlock *succ : getReducedSuccessors(L, current, LI)) {
            if (!visited.insert(succ).second)
                continue;
            predecessor[succ] = current;
            if (succ == dst) {
                std::vector<BasicBlock *> path;
                BasicBlock *cursor = dst;
                path.push_back(cursor);
                while (cursor != src) {
                    auto predIt = predecessor.find(cursor);
                    if (predIt == predecessor.end())
                        return std::nullopt;
                    cursor = predIt->second;
                    path.push_back(cursor);
                }
                std::reverse(path.begin(), path.end());
                return path;
            }
            worklist.push(succ);
        }
    }

    return std::nullopt;
}

static EnumeratedPath makeEnumeratedPath(const std::vector<BasicBlock *> &blocks,
                                         SchematicGraph &graph) {
    EnumeratedPath ep;
    ep.count = 1;
    ep.blocks.reserve(blocks.size());
    for (BasicBlock *BB : blocks)
        ep.blocks.push_back(graph.getOrCreate(BB));
    return ep;
}

static bool coversAllDirectMembers(const LoadedLoopTrace &trace, LoopInfo &LI) {
    for (SchematicBlock *member : trace.members) {
        BasicBlock *BB = member ? member->getLLVMBlock() : nullptr;
        if (!BB)
            continue;
        if (LI.getLoopFor(BB) != trace.loop)
            continue;

        bool covered = false;
        for (const EnumeratedPath &path : trace.iterationPaths) {
            if (std::find(path.blocks.begin(), path.blocks.end(), member) != path.blocks.end()) {
                covered = true;
                break;
            }
        }
        if (!covered)
            return false;
    }
    return true;
}

static bool synthesizeLoopPaths(LoadedLoopTrace &trace, LoopInfo &LI, SchematicGraph &graph,
                                std::string &errorMessage) {
    Loop *L = trace.loop;
    if (!L || !trace.header || !trace.latch) {
        errorMessage = "missing canonical loop header/latch";
        return false;
    }

    unsigned addedPaths = 0;
    BasicBlock *headerBB = L->getHeader();
    BasicBlock *latchBB = L->getLoopLatch();

    for (SchematicBlock *member : trace.members) {
        BasicBlock *memberBB = member ? member->getLLVMBlock() : nullptr;
        if (!memberBB)
            continue;
        if (LI.getLoopFor(memberBB) != L)
            continue;

        bool covered = false;
        for (const EnumeratedPath &path : trace.iterationPaths) {
            if (std::find(path.blocks.begin(), path.blocks.end(), member) != path.blocks.end()) {
                covered = true;
                break;
            }
        }
        if (covered)
            continue;

        auto prefix = findReducedPath(L, headerBB, memberBB, LI);
        auto suffix = findReducedPath(L, memberBB, latchBB, LI);
        if (!prefix || !suffix) {
            errorMessage = "cannot synthesize header-to-latch path covering direct member '" +
                           memberBB->getName().str() + "'";
            return false;
        }

        std::vector<BasicBlock *> combined = *prefix;
        combined.insert(combined.end(), std::next(suffix->begin()), suffix->end());
        EnumeratedPath syntheticPath = makeEnumeratedPath(combined, graph);
        if (!pathAlreadyPresent(trace.iterationPaths, syntheticPath)) {
            trace.iterationPaths.push_back(std::move(syntheticPath));
            addedPaths++;
        }
    }

    if (trace.iterationPaths.empty()) {
        auto canonicalPath = findReducedPath(L, headerBB, latchBB, LI);
        if (!canonicalPath) {
            errorMessage = "cannot synthesize any header-to-latch path for loop '" +
                           headerBB->getName().str() + "'";
            return false;
        }
        trace.iterationPaths.push_back(makeEnumeratedPath(*canonicalPath, graph));
        addedPaths++;
    }

    if (!coversAllDirectMembers(trace, LI)) {
        errorMessage = "synthetic trace augmentation left uncovered direct members in loop '" +
                       headerBB->getName().str() + "'";
        return false;
    }

    if (addedPaths > 0) {
        PLOGW << "TraceLoader: synthesized " << addedPaths << " loop path(s) for '"
              << headerBB->getName() << "'";
    }

    std::sort(trace.iterationPaths.begin(), trace.iterationPaths.end(),
              [](const EnumeratedPath &a, const EnumeratedPath &b) { return a.count > b.count; });
    return true;
}

static std::optional<std::vector<LoadedLoopTrace>>
augmentLoopTraces(const std::vector<LoadedLoopTrace> &parsedLoopTraces, LoopInfo &LI,
                  SchematicGraph &graph) {
    SmallVector<Loop *, 16> loops = LI.getLoopsInPreorder();
    DenseMap<Loop *, size_t> loopToIndex;
    std::vector<LoadedLoopTrace> augmented;
    augmented.reserve(loops.size());

    for (Loop *L : loops) {
        loopToIndex[L] = augmented.size();
        augmented.push_back(makeCanonicalLoopTrace(L, graph));
    }

    for (const LoadedLoopTrace &parsed : parsedLoopTraces) {
        if (!parsed.loop)
            continue;
        auto idxIt = loopToIndex.find(parsed.loop);
        if (idxIt == loopToIndex.end())
            continue;

        LoadedLoopTrace &canonical = augmented[idxIt->second];
        SmallPtrSet<BasicBlock *, 16> memberSet = collectLoopMemberSet(parsed.loop);
        for (const EnumeratedPath &path : parsed.iterationPaths) {
            if (!isStructurallyValidLoopPath(path, canonical, memberSet)) {
                PLOGW << "TraceLoader: dropping malformed loop path for '"
                      << parsed.loop->getHeader()->getName() << "'";
                continue;
            }
            if (!pathAlreadyPresent(canonical.iterationPaths, path))
                canonical.iterationPaths.push_back(path);
        }
    }

    std::stable_sort(
        augmented.begin(), augmented.end(),
        [](const LoadedLoopTrace &a, const LoadedLoopTrace &b) { return a.depth > b.depth; });

    for (LoadedLoopTrace &trace : augmented) {
        std::string errorMessage;
        if (!synthesizeLoopPaths(trace, LI, graph, errorMessage)) {
            PLOGE << "TraceLoader: " << errorMessage;
            return std::nullopt;
        }
    }

    std::stable_sort(
        augmented.begin(), augmented.end(),
        [](const LoadedLoopTrace &a, const LoadedLoopTrace &b) { return a.depth > b.depth; });
    return augmented;
}

} // namespace

TraceLoader::TraceLoader(Function &F, LoopInfo &LI, SchematicGraph &graph)
    : F_(F), LI_(LI), graph_(graph) {
    // Build name -> SchematicBlock* map
    for (BasicBlock &BB : F_) {
        if (BB.hasName())
            nameToBlock_[BB.getName()] = graph_.getOrCreate(&BB);
    }
}

std::optional<LoadedTraces> TraceLoader::load(const std::string &traceFilePath) {
    // Read and parse JSON
    std::ifstream ifs(traceFilePath);
    if (!ifs.is_open()) {
        PLOGE << "TraceLoader: cannot open " << traceFilePath;
        return std::nullopt;
    }

    json root = json::parse(ifs, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        PLOGE << "TraceLoader: JSON parse error in " << traceFilePath;
        return std::nullopt;
    }

    // Look up function name
    std::string funcName = F_.getName().str();
    if (!root.contains(funcName)) {
        PLOGE << "TraceLoader: function '" << funcName << "' not found in trace file";
        return std::nullopt;
    }

    const json &funcObj = root[funcName];
    LoadedTraces result;

    // -----------------------------------------------------------------------
    // Parse function traces
    // -----------------------------------------------------------------------
    if (funcObj.contains("traces") && funcObj["traces"].is_array()) {
        for (const auto &traceObj : funcObj["traces"]) {
            // Accept both our format ("path"/"count") and reference format
            // ("trace"/"nb_execution").
            const char *pathKey = traceObj.contains("path") ? "path" : "trace";
            const char *countKey = traceObj.contains("count") ? "count" : "nb_execution";
            if (!traceObj.contains(pathKey) || !traceObj.contains(countKey))
                continue;

            EnumeratedPath ep;
            bool valid = true;
            for (const auto &bbName : traceObj[pathKey]) {
                std::string name = bbName.get<std::string>();
                auto it = nameToBlock_.find(StringRef(name));
                if (it == nameToBlock_.end()) {
                    PLOGW << "TraceLoader: BB '" << name << "' not found in " << funcName
                          << ", skipping trace";
                    valid = false;
                    break;
                }
                ep.blocks.push_back(it->second);
            }
            if (!valid)
                continue;

            ep.count = traceObj[countKey].get<unsigned>();
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
                PLOGW << "TraceLoader: loop header '" << headerName << "' not found, skipping loop";
                continue;
            }

            LoadedLoopTrace llt;
            llt.header = headerIt->second;
            llt.loop = nullptr;
            llt.latch = nullptr;
            llt.depth = 0;

            // Find matching LLVM Loop*
            for (Loop *L : LI_.getLoopsInPreorder()) {
                if (L->getHeader() == llt.header->getLLVMBlock()) {
                    llt.loop = L;
                    break;
                }
            }

            if (!llt.loop) {
                PLOGW << "TraceLoader: no Loop* for header '" << headerName << "', skipping";
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
            if (!llt.latch) {
                if (BasicBlock *latch = llt.loop->getLoopLatch())
                    llt.latch = graph_.getOrCreate(latch);
            }
            if (llt.depth == 0)
                llt.depth = llt.loop->getLoopDepth();

            // Parse iteration traces
            if (loopObj.contains("traces") && loopObj["traces"].is_array()) {
                for (const auto &traceObj : loopObj["traces"]) {
                    const char *lpPathKey = traceObj.contains("path") ? "path" : "trace";
                    const char *lpCountKey = traceObj.contains("count") ? "count" : "nb_execution";
                    if (!traceObj.contains(lpPathKey) || !traceObj.contains(lpCountKey))
                        continue;

                    EnumeratedPath ep;
                    bool valid = true;
                    for (const auto &bbName : traceObj[lpPathKey]) {
                        std::string name = bbName.get<std::string>();
                        auto it = nameToBlock_.find(StringRef(name));
                        if (it == nameToBlock_.end()) {
                            PLOGW << "TraceLoader: BB '" << name
                                  << "' in loop trace not found, skipping";
                            valid = false;
                            break;
                        }
                        ep.blocks.push_back(it->second);
                    }
                    if (!valid)
                        continue;

                    ep.count = traceObj[lpCountKey].get<unsigned>();
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

    auto augmentedLoopTraces = augmentLoopTraces(result.loopTraces, LI_, graph_);
    if (!augmentedLoopTraces)
        return std::nullopt;
    result.loopTraces = std::move(*augmentedLoopTraces);

    PLOGI << "TraceLoader: loaded " << result.functionPaths.size() << " function traces, "
          << result.loopTraces.size() << " loop traces for " << funcName;

    return result;
}

} // namespace checkpoint
