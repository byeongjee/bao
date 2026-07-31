#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/BasicBlock.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace checkpoint {

/// Names of the synthetic per-function boundary blocks prepended/appended to
/// every function trace (ref: schematic.py %START_/%END_, trace.py:288-289).
/// Shared so the RCG seed can recognize the function-entry sub-path and charge
/// call_cost exactly once (ref: schematic.py:184-186).
inline constexpr llvm::StringRef kStartFuncName = "START_Func";
inline constexpr llvm::StringRef kEndFuncName = "END_Func";

/// Names of the synthetic per-loop boundary blocks (ref: schematic.py %START_Loop
/// / %END_Loop, loop_utils.py). The reference subtracts call_cost on the loop RCG
/// seed too (cfg.first_bb == trace[0] holds for the loop cfg), so the RCG charges
/// call_cost when the sub-path front is START_Func OR START_Loop.
inline constexpr llvm::StringRef kStartLoopName = "START_Loop";
inline constexpr llvm::StringRef kEndLoopName = "END_Loop";

class SchematicBlock {
  public:
    explicit SchematicBlock(llvm::BasicBlock *bb);
    explicit SchematicBlock(std::string name);

    llvm::BasicBlock *getLLVMBlock() const { return bb_; }
    llvm::StringRef getName() const { return name_; }
    bool isSynthetic() const { return bb_ == nullptr; }

    /// Name for diagnostics: the live LLVM block name (name_ is cached at
    /// construction and can go stale if the block is renamed), or the cached
    /// name for synthetic blocks.
    std::string displayName() const { return bb_ ? bb_->getName().str() : name_; }

    const std::vector<SchematicBlock *> &predecessors() const { return preds_; }
    const std::vector<SchematicBlock *> &successors() const { return succs_; }

    void addPredecessor(SchematicBlock *b) {
        if (std::find(preds_.begin(), preds_.end(), b) == preds_.end())
            preds_.push_back(b);
    }
    void addSuccessor(SchematicBlock *b) {
        if (std::find(succs_.begin(), succs_.end(), b) == succs_.end())
            succs_.push_back(b);
    }

  private:
    llvm::BasicBlock *bb_;
    std::string name_;
    std::vector<SchematicBlock *> preds_;
    std::vector<SchematicBlock *> succs_;
};

class SchematicGraph {
  public:
    /// Get or create a SchematicBlock for a real BasicBlock (deduplicated).
    SchematicBlock *getOrCreate(llvm::BasicBlock *bb);

    /// Create a new synthetic block (not deduplicated — each call creates a fresh node).
    SchematicBlock *createSynthetic(const std::string &name);

    /// Add directed edges between adjacent elements in a trace.
    /// For trace [A, B, C], adds edges A->B and B->C.
    void addTraceEdges(const std::vector<SchematicBlock *> &trace);

    /// Populate predecessor/successor edges from the real LLVM CFG.
    /// Must be called after all real blocks are created (or lazily creates them).
    void addCFGEdges(llvm::Function &F);

  private:
    llvm::DenseMap<llvm::BasicBlock *, std::unique_ptr<SchematicBlock>> realBlocks_;
    std::vector<std::unique_ptr<SchematicBlock>> syntheticBlocks_;
};

} // namespace checkpoint
