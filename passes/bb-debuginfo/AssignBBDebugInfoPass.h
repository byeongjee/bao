#pragma once

#include "llvm/IR/PassManager.h"

namespace bbdebuginfo {

/// LLVM pass that assigns unique debug locations to each basic block.
/// Uses line number to encode BB index (0, 1, 2, ...).
/// Column is set to 0; scope is the function's DISubprogram.
///
/// This enables mapping assembly back to IR basic blocks via DWARF.
/// All non-PHI instructions in a basic block receive the same DILocation,
/// effectively "painting" the entire block with a single identity.
///
/// Why we label all instructions (not just one per BB):
/// - DWARF line table is built from instruction-level debug locations
/// - Only labeled instructions appear in the DWARF line table
/// - Unlabeled instructions might inherit wrong locations from surrounding code
/// - Complete coverage ensures reliable BB-to-address mapping
class AssignBBDebugInfoPass : public llvm::PassInfoMixin<AssignBBDebugInfoPass> {
public:
    llvm::PreservedAnalyses run(llvm::Function &F,
                                 llvm::FunctionAnalysisManager &AM);
    static llvm::StringRef name() { return "AssignBBDebugInfoPass"; }
};

} // namespace bbdebuginfo
