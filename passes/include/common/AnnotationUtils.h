#pragma once

#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

#include <string>

namespace checkpoint {

/// Extract the annotation string from a constant operand of
/// llvm.global.annotations.  Returns "" on failure.
std::string extractAnnotationString(llvm::Constant *AnnoOp);

/// Return true if \p GV carries an \c annotate("milp_candidate") attribute
/// in \p M's \c llvm.global.annotations metadata.
bool isMilpCandidateAnnotated(llvm::GlobalVariable *GV, llvm::Module *M);

} // namespace checkpoint
