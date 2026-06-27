#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"

#include <vector>

namespace llvm {
class Function;
class BasicBlock;
class CallInst;
class Instruction;
class Module;
} // namespace llvm

namespace checkpoint {

/// Metadata kinds marking the isolated call site. Set on the CALL instruction
/// (entry) and on the empty exit block's terminator (exit). They round-trip
/// through `opt -S`, so the separately-invoked solve pass can recognize the
/// split blocks after serialization.
inline constexpr llvm::StringRef kSchematicCallEntryMD = "schematic.call_entry";
inline constexpr llvm::StringRef kSchematicCallExitMD = "schematic.call_exit";

/// A callee that must NOT be isolated (kept inline-cost within its block),
/// mirroring the reference's builtin ignore_list (cfg_modification.py:140-145)
/// and SchematicStateAnalysis's whitelist.
bool isSchematicHelperCallee(const llvm::Function &F);

/// True if I is an isolated call (a CallBase carrying kSchematicCallEntryMD).
bool isIsolatedCallEntry(const llvm::Instruction &I);

/// One isolated call site, recovered from metadata.
struct IsolatedCall {
    llvm::BasicBlock *entry = nullptr; // block holding the (marked) call
    llvm::BasicBlock *exit = nullptr;  // empty block after the call
    llvm::Function *callee = nullptr;
    llvm::CallInst *call = nullptr;
};

/// Recover all isolated call sites in F from their metadata.
std::vector<IsolatedCall> collectIsolatedCalls(llvm::Function &F);

/// Module pass (name: "schematic-isolate"). Splits every isolatable call into
/// pre -> call_entry(call) -> call_exit(empty) -> post and marks call_entry /
/// call_exit with metadata. Rejects recursion (cycle in the defined-function
/// call graph) with a fatal error. Faithful port of cfg_modification.py:45-153
/// + the recursion check in __init__.py:115-117.
class CallIsolationPass : public llvm::PassInfoMixin<CallIsolationPass> {
  public:
    llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
    static llvm::StringRef name() { return "CallIsolationPass"; }
};

} // namespace checkpoint
