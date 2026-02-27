#ifndef CHECKPOINT_BLOCK_UTILS_H
#define CHECKPOINT_BLOCK_UTILS_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <string>
#include <sys/resource.h>

namespace checkpoint {

/// Get a unique name for a basic block.
/// If the block has a name, returns it.
/// Otherwise, returns "bbN" where N is the block's index in the function.
inline std::string getBlockName(const llvm::BasicBlock &BB,
                                const llvm::Function &F) {
    std::string name = BB.getName().str();
    if (!name.empty()) {
        return name;
    }

    // Unnamed block - compute index
    size_t idx = 0;
    for (const llvm::BasicBlock &B : F) {
        if (&B == &BB) {
            return "bb" + std::to_string(idx);
        }
        idx++;
    }

    // Should never reach here
    return "bb_unknown";
}

/// Get a unique name for a basic block pointer.
/// Convenience overload that dereferences the pointer.
inline std::string getBlockName(const llvm::BasicBlock *BB,
                                const llvm::Function &F) {
    return getBlockName(*BB, F);
}

/// Get peak resident set size in KB.
/// Returns -1 on failure.
inline long getPeakRSSKb() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    long kb = usage.ru_maxrss;
#ifdef __APPLE__
    kb /= 1024;  // macOS reports bytes; Linux reports KB
#endif
    return kb;
}

} // namespace checkpoint

#endif // CHECKPOINT_BLOCK_UTILS_H
