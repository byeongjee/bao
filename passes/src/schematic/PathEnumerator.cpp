#include "schematic/PathEnumerator.h"

namespace checkpoint {

PathEnumerator::PathEnumerator(llvm::Function &F,
                               llvm::BranchProbabilityInfo &BPI,
                               llvm::LoopInfo &LI,
                               unsigned maxPaths)
    : F_(F), BPI_(BPI), LI_(LI), maxPaths_(maxPaths) {}

std::vector<EnumeratedPath> PathEnumerator::enumerate() {
    return {};
}

std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>>
PathEnumerator::identifyBackEdges() const {
    return {};
}

std::vector<EnumeratedPath> PathEnumerator::generateCoveragePaths(
    const std::set<llvm::BasicBlock *> &uncovered,
    const std::set<std::pair<llvm::BasicBlock *, llvm::BasicBlock *>> &backEdges) const {
    return {};
}

} // namespace checkpoint
