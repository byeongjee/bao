#include "MaxCheckpointCounter.h"

#include "llvm/IR/CFG.h"

#include <algorithm>
#include <functional>

namespace checkpoint {

MaxCheckpointCounter::MaxCheckpointCounter(
    llvm::Function &F,
    llvm::LoopInfo &LI,
    const std::set<llvm::BasicBlock*> &checkpoints)
    : F_(F), LI_(LI), checkpoints_(checkpoints) {}

void MaxCheckpointCounter::setLoopBounds(
    const std::map<const llvm::Loop*, unsigned> &bounds) {
    loopBounds_ = bounds;
}

void MaxCheckpointCounter::findBackEdges() {
    backEdges_.clear();
    backEdgeIndex_.clear();

    std::set<llvm::BasicBlock*> visited;
    std::set<llvm::BasicBlock*> inStack;

    // DFS to find back-edges
    std::function<void(llvm::BasicBlock*)> dfs = [&](llvm::BasicBlock *BB) {
        visited.insert(BB);
        inStack.insert(BB);

        for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
            if (inStack.count(Succ)) {
                // Back-edge found: BB (latch) -> Succ (header)
                backEdges_.push_back({BB, Succ});
            } else if (!visited.count(Succ)) {
                dfs(Succ);
            }
        }

        inStack.erase(BB);
    };

    llvm::BasicBlock &Entry = F_.getEntryBlock();
    dfs(&Entry);

    // Build index map
    for (size_t i = 0; i < backEdges_.size(); ++i) {
        backEdgeIndex_[backEdges_[i]] = static_cast<int>(i);
    }
}

bool MaxCheckpointCounter::isBackEdge(llvm::BasicBlock *From,
                                       llvm::BasicBlock *To) const {
    return backEdgeIndex_.count({From, To}) > 0;
}

unsigned MaxCheckpointCounter::getLoopBound(llvm::Loop *L) const {
    auto it = loopBounds_.find(L);
    if (it != loopBounds_.end()) {
        return it->second;
    }
    return defaultBound_;
}

int MaxCheckpointCounter::countFromBlock(
    llvm::BasicBlock *BB,
    std::vector<int> &edgeCounts,
    std::vector<llvm::BasicBlock*> &path) {

    // Create memo key: (BasicBlock*, edgeCounts as tuple-like)
    // We use a map with pair<BasicBlock*, vector<int>> as key
    auto memoKey = std::make_pair(BB, edgeCounts);
    auto memoIt = memo_.find(memoKey);
    if (memoIt != memo_.end()) {
        // Found in cache - but we can't restore the path easily
        // Just return the cached count
        return memoIt->second;
    }

    int checkpointHere = checkpoints_.count(BB) ? 1 : 0;
    path.push_back(BB);

    // Check if this is an exit block (no successors)
    if (llvm::succ_empty(BB)) {
        memo_[memoKey] = checkpointHere;
        return checkpointHere;
    }

    int best = 0;
    std::vector<llvm::BasicBlock*> bestPath;

    for (llvm::BasicBlock *Succ : llvm::successors(BB)) {
        if (isBackEdge(BB, Succ)) {
            // This is a back-edge (BB=latch, Succ=header)
            int idx = backEdgeIndex_.at({BB, Succ});

            // Get the loop for this back-edge
            llvm::Loop *L = LI_.getLoopFor(Succ);
            unsigned bound = getLoopBound(L);

            // Check if we've exceeded the iteration limit
            if (static_cast<unsigned>(edgeCounts[idx]) >= bound) {
                continue;  // Don't traverse this back-edge again
            }

            // Increment count and recurse
            edgeCounts[idx]++;
            int count = countFromBlock(Succ, edgeCounts, path);
            edgeCounts[idx]--;

            if (count > best) {
                best = count;
                bestPath = path;
            }
        } else {
            // Regular edge
            int count = countFromBlock(Succ, edgeCounts, path);
            if (count > best) {
                best = count;
                bestPath = path;
            }
        }
    }

    path.pop_back();
    if (!bestPath.empty()) {
        path = std::move(bestPath);
    }

    int result = checkpointHere + best;
    memo_[memoKey] = result;
    return result;
}

CountResult MaxCheckpointCounter::compute() {
    // Find all back-edges first
    findBackEdges();

    // Clear memo for fresh computation
    memo_.clear();

    // Initialize edge counts to zero
    std::vector<int> edgeCounts(backEdges_.size(), 0);
    std::vector<llvm::BasicBlock*> path;

    llvm::BasicBlock &Entry = F_.getEntryBlock();
    int maxCount = countFromBlock(&Entry, edgeCounts, path);

    return CountResult{maxCount, std::move(path)};
}

} // namespace checkpoint
