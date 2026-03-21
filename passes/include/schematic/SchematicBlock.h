#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace checkpoint {

class SchematicBlock {
  public:
    explicit SchematicBlock(llvm::BasicBlock *bb);
    explicit SchematicBlock(std::string name);

    llvm::BasicBlock *getLLVMBlock() const { return bb_; }
    llvm::StringRef getName() const { return name_; }
    bool isSynthetic() const { return bb_ == nullptr; }

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

  private:
    llvm::DenseMap<llvm::BasicBlock *, std::unique_ptr<SchematicBlock>> realBlocks_;
    std::vector<std::unique_ptr<SchematicBlock>> syntheticBlocks_;
};

} // namespace checkpoint
