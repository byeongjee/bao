#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"

#include <optional>
#include <string>

namespace checkpoint {

/// Loads per-BB visit counts from a JSON file produced by the BB frequency
/// collection runtime. Resolves BB names to BasicBlock* pointers for a
/// given function.
class BBFreqLoader {
  public:
    /// Parse JSON and resolve BB names for a given function.
    /// Returns std::nullopt on error (file not found, parse error,
    /// function not in file).
    static std::optional<BBFreqLoader> load(const std::string &jsonPath, llvm::Function &F);

    /// Get the raw visit count for a block.
    /// Returns std::nullopt if the block was not in the frequency file.
    std::optional<uint64_t> getBlockCount(const llvm::BasicBlock *BB) const;

    /// Get all loaded counts (for diagnostics).
    const llvm::DenseMap<const llvm::BasicBlock *, uint64_t> &getCounts() const { return counts_; }

  private:
    llvm::DenseMap<const llvm::BasicBlock *, uint64_t> counts_;
};

} // namespace checkpoint
