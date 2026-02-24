#pragma once

#include "llvm/IR/Function.h"

namespace checkpoint {

/// Assign deterministic names to any unnamed basic blocks in F.
/// Named BBs are left untouched. Unnamed BBs are named "bb.0", "bb.1", ...
/// The index counter increments for every BB (not just unnamed ones) so the
/// numbering is stable regardless of which BBs already have names.
/// Both trace-collect and schematic pipelines must call this on the same IR
/// to ensure consistent names.
void ensureBBNames(llvm::Function &F);

} // namespace checkpoint
