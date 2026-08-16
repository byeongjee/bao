#pragma once

#include "llvm/IR/PassManager.h"

#include <map>

namespace llvm {
class Function;
} // namespace llvm

namespace checkpoint {

struct SchematicParams;
struct CallSummary;
class VMAddressTracker;

/// LLVM module pass for SCHEMATIC checkpoint insertion with inter-procedural
/// (function-call) support. Runs bottom-up over the call graph: each callee is
/// solved before its callers so its summary can be folded into the caller's
/// call sites (ref: schematic.py:676-695). Single-function modules reduce to the
/// former per-function behavior.
class SchematicPass : public llvm::PassInfoMixin<SchematicPass> {
  public:
    /// Module driver.
    llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);

    /// Pass name for registration.
    static llvm::StringRef name() { return "SchematicPass"; }

  private:
    /// Solve a single function (the former per-function pass body). Per-function
    /// analyses come from the passed FunctionAnalysisManager; `params` is parsed
    /// once by the driver and `sharedVMTracker` is program-wide. `summaries`
    /// holds already-solved callee summaries for folding at call sites. On
    /// success the function is instrumented, `out` is filled, and true is
    /// returned; a benign skip or hard failure returns false (no summary).
    bool solveFunction(llvm::Function &F, llvm::FunctionAnalysisManager &FAM,
                       const SchematicParams &params, VMAddressTracker &sharedVMTracker,
                       const std::map<llvm::Function *, CallSummary> &summaries, CallSummary &out);
};

} // namespace checkpoint
