#ifndef CHECKPOINT_BLOCK_UTILS_H
#define CHECKPOINT_BLOCK_UTILS_H

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <iterator>
#include <string>
#include <sys/resource.h>

namespace checkpoint {

/// Get a unique name for a basic block.
/// If the block has a name, returns it.
/// Otherwise, returns "bbN" where N is the block's index in the function.
inline std::string getBlockName(const llvm::BasicBlock &BB, const llvm::Function &F) {
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
inline std::string getBlockName(const llvm::BasicBlock *BB, const llvm::Function &F) {
    return getBlockName(*BB, F);
}

/// Return an insert point after all PHIs and AllocaInsts in a block.
/// Falls back to before the terminator if the block is otherwise empty.
inline llvm::BasicBlock::iterator getInsertPointAfterAllocas(llvm::BasicBlock &BB) {
    auto it = BB.getFirstNonPHIIt();
    while (it != BB.end() && llvm::isa<llvm::AllocaInst>(&*it))
        ++it;
    if (it == BB.end())
        it = std::prev(BB.end());
    return it;
}

/// Get peak resident set size in KB.
/// Returns -1 on failure.
inline long getPeakRSSKb() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return -1;
    long kb = usage.ru_maxrss;
#ifdef __APPLE__
    kb /= 1024; // macOS reports bytes; Linux reports KB
#endif
    return kb;
}

} // namespace checkpoint

#endif // CHECKPOINT_BLOCK_UTILS_H
