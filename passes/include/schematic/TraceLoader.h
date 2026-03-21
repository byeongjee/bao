#pragma once

#include "schematic/SchematicBlock.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <optional>
#include <string>
#include <vector>

namespace checkpoint {

/// A single entry-to-exit (or header-to-latch) path with execution metadata.
struct EnumeratedPath {
    std::vector<SchematicBlock *> blocks;
    unsigned count = 0; // execution count from trace (0 = unobserved)
};

struct LoadedLoopTrace {
    llvm::Loop *loop;
    SchematicBlock *header;
    std::vector<SchematicBlock *> members;
    SchematicBlock *latch;
    unsigned depth;
    std::vector<EnumeratedPath> iterationPaths; // each has blocks + count
};

struct LoadedTraces {
    std::vector<EnumeratedPath> functionPaths; // entry-to-exit, loops collapsed
    std::vector<LoadedLoopTrace> loopTraces;
};

class TraceLoader {
  public:
    TraceLoader(llvm::Function &F, llvm::LoopInfo &LI, SchematicGraph &graph);

    /// Load traces from a JSON file. Returns nullopt if the function
    /// is not found in the trace file or the file cannot be parsed.
    std::optional<LoadedTraces> load(const std::string &traceFilePath);

  private:
    llvm::Function &F_;
    llvm::LoopInfo &LI_;
    SchematicGraph &graph_;
    llvm::DenseMap<llvm::StringRef, SchematicBlock *> nameToBlock_;
};

} // namespace checkpoint
