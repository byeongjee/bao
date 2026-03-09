#pragma once

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
    std::vector<llvm::BasicBlock *> blocks;
    unsigned count = 0; // execution count from trace (0 = unobserved)
};

struct LoadedLoopTrace {
    llvm::Loop *loop;
    llvm::BasicBlock *header;
    std::vector<llvm::BasicBlock *> members;
    llvm::BasicBlock *latch;
    unsigned depth;
    std::vector<EnumeratedPath> iterationPaths; // each has blocks + count
};

struct LoadedTraces {
    std::vector<EnumeratedPath> functionPaths; // entry-to-exit, loops collapsed
    std::vector<LoadedLoopTrace> loopTraces;
};

class TraceLoader {
  public:
    TraceLoader(llvm::Function &F, llvm::LoopInfo &LI);

    /// Load traces from a JSON file. Returns nullopt if the function
    /// is not found in the trace file or the file cannot be parsed.
    std::optional<LoadedTraces> load(const std::string &traceFilePath);

  private:
    llvm::Function &F_;
    llvm::LoopInfo &LI_;
    llvm::DenseMap<llvm::StringRef, llvm::BasicBlock *> nameToBlock_;
};

} // namespace checkpoint
