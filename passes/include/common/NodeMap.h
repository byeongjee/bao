#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/ValueHandle.h"

#include <cstdint>
#include <limits>

namespace checkpoint {

using NodeId = uint32_t;
constexpr NodeId kInvalidNodeId = std::numeric_limits<NodeId>::max();

/// Minimal node map for abstract-node to concrete-block resolution.
///
/// Required maps:
/// - concreteNodes_: NodeId -> concrete block handle
/// - reverseConcrete_: concrete block -> NodeId
/// - summaryRepresentative_: summary NodeId -> representative concrete block
class NodeMap {
  public:
    void setConcreteNode(NodeId node, llvm::BasicBlock *BB) {
        if (!BB) {
            return;
        }
        concreteNodes_[node] = BB;
        reverseConcrete_[BB] = node;
    }

    void setSummaryRepresentative(NodeId summaryNode, llvm::BasicBlock *representativeBB) {
        if (!representativeBB) {
            return;
        }
        summaryRepresentative_[summaryNode] = representativeBB;
    }

    void setSummaryMember(NodeId summaryNode, llvm::BasicBlock *BB) {
        if (!BB)
            return;
        reverseSummaryMembers_[BB] = summaryNode;
    }

    NodeId getNodeId(const llvm::BasicBlock *BB) const {
        auto it = reverseConcrete_.find(BB);
        if (it != reverseConcrete_.end())
            return it->second;
        auto it2 = reverseSummaryMembers_.find(BB);
        if (it2 != reverseSummaryMembers_.end())
            return it2->second;
        return kInvalidNodeId;
    }

    llvm::BasicBlock *getConcreteBlock(NodeId node) const {
        auto itConcrete = concreteNodes_.find(node);
        if (itConcrete != concreteNodes_.end()) {
            return llvm::dyn_cast_or_null<llvm::BasicBlock>(itConcrete->second);
        }

        auto itSummary = summaryRepresentative_.find(node);
        if (itSummary != summaryRepresentative_.end()) {
            return llvm::dyn_cast_or_null<llvm::BasicBlock>(itSummary->second);
        }

        return nullptr;
    }

  private:
    llvm::DenseMap<NodeId, llvm::WeakTrackingVH> concreteNodes_;
    llvm::DenseMap<const llvm::BasicBlock *, NodeId> reverseConcrete_;
    llvm::DenseMap<NodeId, llvm::WeakTrackingVH> summaryRepresentative_;
    llvm::DenseMap<const llvm::BasicBlock *, NodeId> reverseSummaryMembers_;
};

} // namespace checkpoint
