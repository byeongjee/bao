#include "schematic/PathEnumerator.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CFG.h"

#include <algorithm>
#include <map>
#include <queue>

namespace checkpoint {

PathEnumerator::PathEnumerator(llvm::Function &F,
                               llvm::BranchProbabilityInfo &BPI,
                               llvm::LoopInfo &LI,
                               unsigned maxPaths)
    : F_(F), BPI_(BPI), LI_(LI), maxPaths_(maxPaths) {}

std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>>
PathEnumerator::identifyBackEdges() const {
    std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> backEdges;
    for (llvm::Loop *L : LI_.getLoopsInPreorder()) {
        llvm::BasicBlock *header = L->getHeader();
        llvm::SmallVector<llvm::BasicBlock *, 4> latches;
        L->getLoopLatches(latches);
        for (llvm::BasicBlock *latch : latches)
            backEdges.insert({latch, header});
    }
    return backEdges;
}

std::vector<EnumeratedPath> PathEnumerator::enumerate() {
    auto backEdges = identifyBackEdges();
    std::set<llvm::BasicBlock *> coveredBlocks;
    std::vector<EnumeratedPath> completePaths;

    // Collect all reachable blocks for coverage tracking
    std::set<llvm::BasicBlock *> allBlocks;
    for (llvm::BasicBlock &BB : F_)
        allBlocks.insert(&BB);

    // Identify exit blocks (no non-back-edge successors, or return blocks)
    auto isExitBlock = [&](llvm::BasicBlock *BB) -> bool {
        if (llvm::isa<llvm::ReturnInst>(BB->getTerminator()))
            return true;
        if (llvm::isa<llvm::UnreachableInst>(BB->getTerminator()))
            return true;
        // Check if all successors are via back-edges
        bool hasNonBackEdgeSucc = false;
        for (llvm::BasicBlock *succ : llvm::successors(BB)) {
            if (backEdges.count({BB, succ}) == 0) {
                hasNonBackEdgeSucc = true;
                break;
            }
        }
        return !hasNonBackEdgeSucc;
    };

    // Priority queue: (probability, partial path)
    // Use negative probability for max-heap behavior with std::priority_queue
    struct PartialPath {
        std::vector<llvm::BasicBlock *> blocks;
        double probability;
        std::set<llvm::BasicBlock *> visited; // cycle detection on DAG

        bool operator<(const PartialPath &o) const {
            return probability < o.probability; // max-heap
        }
    };

    std::priority_queue<PartialPath> pq;

    // Seed with entry block
    llvm::BasicBlock *entry = &F_.getEntryBlock();
    PartialPath seed;
    seed.blocks.push_back(entry);
    seed.probability = 1.0;
    seed.visited.insert(entry);
    pq.push(std::move(seed));

    while (!pq.empty() &&
           completePaths.size() < maxPaths_) {
        PartialPath current = pq.top();
        pq.pop();

        llvm::BasicBlock *lastBB = current.blocks.back();

        if (isExitBlock(lastBB)) {
            // Complete path
            EnumeratedPath ep;
            ep.blocks = std::move(current.blocks);
            ep.probability = current.probability;
            for (auto *BB : ep.blocks)
                coveredBlocks.insert(BB);
            completePaths.push_back(std::move(ep));
            continue;
        }

        // Extend path along non-back-edge successors
        unsigned succIdx = 0;
        for (llvm::BasicBlock *succ : llvm::successors(lastBB)) {
            if (backEdges.count({lastBB, succ}) == 0 &&
                current.visited.count(succ) == 0) {
                // Compute edge probability
                llvm::BranchProbability bp =
                    BPI_.getEdgeProbability(lastBB, succIdx);
                double edgeProb = static_cast<double>(bp.getNumerator()) /
                                  static_cast<double>(bp.getDenominator());

                PartialPath extended;
                extended.blocks = current.blocks;
                extended.blocks.push_back(succ);
                extended.probability = current.probability * edgeProb;
                extended.visited = current.visited;
                extended.visited.insert(succ);
                pq.push(std::move(extended));
            }
            succIdx++;
        }
    }

    // Check coverage
    bool allCovered = true;
    for (llvm::BasicBlock *BB : allBlocks) {
        if (coveredBlocks.count(BB) == 0) {
            allCovered = false;
            break;
        }
    }

    // Generate coverage paths for uncovered blocks
    if (!allCovered) {
        std::set<llvm::BasicBlock *> uncovered;
        for (llvm::BasicBlock *BB : allBlocks) {
            if (coveredBlocks.count(BB) == 0)
                uncovered.insert(BB);
        }
        auto coveragePaths = generateCoveragePaths(uncovered, backEdges);
        for (auto &cp : coveragePaths) {
            completePaths.push_back(std::move(cp));
            // Update coverage
            for (auto *BB : completePaths.back().blocks)
                coveredBlocks.insert(BB);
        }
    }

    // Sort by decreasing probability
    std::sort(completePaths.begin(), completePaths.end(),
              [](const EnumeratedPath &a, const EnumeratedPath &b) {
                  return a.probability > b.probability;
              });

    return completePaths;
}

std::vector<EnumeratedPath> PathEnumerator::generateCoveragePaths(
    const std::set<llvm::BasicBlock *> &uncovered,
    const std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> &backEdges) const {

    std::vector<EnumeratedPath> result;
    llvm::BasicBlock *entry = &F_.getEntryBlock();

    auto isExitBlock = [&](llvm::BasicBlock *BB) -> bool {
        if (llvm::isa<llvm::ReturnInst>(BB->getTerminator()))
            return true;
        if (llvm::isa<llvm::UnreachableInst>(BB->getTerminator()))
            return true;
        bool hasNonBackEdgeSucc = false;
        for (llvm::BasicBlock *succ : llvm::successors(BB)) {
            if (backEdges.count({BB, succ}) == 0) {
                hasNonBackEdgeSucc = true;
                break;
            }
        }
        return !hasNonBackEdgeSucc;
    };

    for (llvm::BasicBlock *target : uncovered) {
        // BFS from entry to target on DAG
        std::map<llvm::BasicBlock *, llvm::BasicBlock *> predToTarget;
        std::queue<llvm::BasicBlock *> bfsQ;
        bfsQ.push(entry);
        predToTarget[entry] = nullptr;
        bool foundTarget = false;

        while (!bfsQ.empty() && !foundTarget) {
            llvm::BasicBlock *cur = bfsQ.front();
            bfsQ.pop();
            if (cur == target) { foundTarget = true; break; }
            for (llvm::BasicBlock *succ : llvm::successors(cur)) {
                if (backEdges.count({cur, succ}) == 0 &&
                    predToTarget.count(succ) == 0) {
                    predToTarget[succ] = cur;
                    bfsQ.push(succ);
                }
            }
        }

        if (!foundTarget) continue;

        // Reconstruct path from entry to target
        std::vector<llvm::BasicBlock *> pathToTarget;
        for (llvm::BasicBlock *bb = target; bb != nullptr; bb = predToTarget[bb])
            pathToTarget.push_back(bb);
        std::reverse(pathToTarget.begin(), pathToTarget.end());

        // BFS from target to any exit
        std::map<llvm::BasicBlock *, llvm::BasicBlock *> predToExit;
        std::queue<llvm::BasicBlock *> bfsQ2;
        bfsQ2.push(target);
        predToExit[target] = nullptr;
        llvm::BasicBlock *exitBB = nullptr;

        if (isExitBlock(target)) {
            exitBB = target;
        } else {
            while (!bfsQ2.empty() && !exitBB) {
                llvm::BasicBlock *cur = bfsQ2.front();
                bfsQ2.pop();
                for (llvm::BasicBlock *succ : llvm::successors(cur)) {
                    if (backEdges.count({cur, succ}) == 0 &&
                        predToExit.count(succ) == 0) {
                        predToExit[succ] = cur;
                        if (isExitBlock(succ)) {
                            exitBB = succ;
                            break;
                        }
                        bfsQ2.push(succ);
                    }
                }
            }
        }

        if (!exitBB) continue;

        // Reconstruct path from target to exit (skip target itself, already in pathToTarget)
        if (exitBB != target) {
            std::vector<llvm::BasicBlock *> pathToExit;
            for (llvm::BasicBlock *bb = exitBB; bb != nullptr; bb = predToExit[bb])
                pathToExit.push_back(bb);
            std::reverse(pathToExit.begin(), pathToExit.end());

            // Combine: pathToTarget + pathToExit (skip duplicate target)
            for (size_t i = 1; i < pathToExit.size(); ++i)
                pathToTarget.push_back(pathToExit[i]);
        }

        // Compute path probability
        double prob = 1.0;
        for (size_t i = 0; i + 1 < pathToTarget.size(); ++i) {
            llvm::BasicBlock *src = pathToTarget[i];
            llvm::BasicBlock *dst = pathToTarget[i + 1];
            unsigned succIdx = 0;
            for (llvm::BasicBlock *s : llvm::successors(src)) {
                if (s == dst) break;
                succIdx++;
            }
            llvm::BranchProbability bp =
                BPI_.getEdgeProbability(src, succIdx);
            prob *= static_cast<double>(bp.getNumerator()) /
                    static_cast<double>(bp.getDenominator());
        }

        EnumeratedPath ep;
        ep.blocks = std::move(pathToTarget);
        ep.probability = prob;
        result.push_back(std::move(ep));
    }

    // Sort coverage paths by decreasing probability
    std::sort(result.begin(), result.end(),
              [](const EnumeratedPath &a, const EnumeratedPath &b) {
                  return a.probability > b.probability;
              });

    return result;
}

} // namespace checkpoint
