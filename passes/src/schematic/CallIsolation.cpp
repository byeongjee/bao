#include "schematic/CallIsolation.h"

#include "common/FunctionFilters.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <string>

using namespace llvm;

namespace checkpoint {

bool isSchematicHelperCallee(const Function &F) {
    if (F.isIntrinsic())
        return true;
    StringRef N = F.getName();
    if (N == "__loop_tripcount")
        return true;
    if (N == "bench_halt")
        return true;
    // Benchmark-infrastructure functions (timing/IO/debug) are skipped by the
    // module driver, so they must not be isolated either — otherwise their calls
    // would be isolated yet never summarized, stranding the caller. Keeping the
    // three whitelists (isolation / StateAnalysis / driver-skip) consistent.
    if (isBenchmarkInfrastructureFunction(N))
        return true;
    if (N.starts_with("__mspabi_") || N.starts_with("__aeabi_") || N.starts_with("__div") ||
        N.starts_with("__udiv") || N.starts_with("__mul") || N.starts_with("__mod"))
        return true;
    return false;
}

bool isIsolatedCallEntry(const Instruction &I) {
    return isa<CallBase>(I) && I.getMetadata(kSchematicCallEntryMD) != nullptr;
}

namespace {

/// A direct call to a defined, non-helper function — the reference's isolatable
/// CallOperation (cfg_modification.py:140-145).
bool isIsolatableCall(const CallInst &CI) {
    if (CI.isInlineAsm())
        return false;
    Function *callee = CI.getCalledFunction();
    if (!callee || callee->isDeclaration())
        return false;
    if (isSchematicHelperCallee(*callee))
        return false;
    return true;
}

MDNode *makeGroupMD(LLVMContext &Ctx, unsigned gid) {
    return MDNode::get(Ctx,
                       {ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(Ctx), gid))});
}

unsigned getGroupId(const MDNode *md) {
    auto *cam = cast<ConstantAsMetadata>(md->getOperand(0));
    return static_cast<unsigned>(cast<ConstantInt>(cam->getValue())->getZExtValue());
}

/// Split the block holding CI into  pre -> call_entry(CI) -> call_exit() -> post.
/// Reference: create_bb_for_function (cfg_modification.py:45-113).
void splitOneCall(CallInst *CI, unsigned gid) {
    LLVMContext &Ctx = CI->getContext();
    StringRef callee = CI->getCalledFunction()->getName();
    BasicBlock *pre = CI->getParent();

    // call_entry: original block keeps the pre-call instructions; the new block
    // begins at the call.
    BasicBlock *entry = pre->splitBasicBlock(CI, Twine("ci.") + callee + ".entry");
    // post: split right after the call so call_entry == [call, br].
    Instruction *afterCall = CI->getNextNode();
    BasicBlock *post = entry->splitBasicBlock(afterCall, Twine("ci.") + callee + ".cont");
    // call_exit: empty block inserted on the call_entry -> post edge.
    BasicBlock *exit =
        SplitEdge(entry, post, nullptr, nullptr, nullptr, Twine("ci.") + callee + ".exit");

    MDNode *md = makeGroupMD(Ctx, gid);
    CI->setMetadata(kSchematicCallEntryMD, md);
    exit->getTerminator()->setMetadata(kSchematicCallExitMD, md);
}

/// Isolate every isolatable call in F. Restarts the scan after each split since
/// splitting invalidates iterators and moves the post-call tail into a new block
/// (mirrors the reference re-pushing next_bb, cfg_modification.py:151).
unsigned isolateInFunction(Function &F, unsigned &gid) {
    unsigned count = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                auto *CI = dyn_cast<CallInst>(&I);
                if (CI && !CI->getMetadata(kSchematicCallEntryMD) && isIsolatableCall(*CI)) {
                    splitOneCall(CI, gid++);
                    ++count;
                    changed = true;
                    break;
                }
            }
            if (changed)
                break;
        }
    }
    return count;
}

/// DFS cycle detection over the defined-function direct-call graph.
/// color: 0=white, 1=gray, 2=black.
bool dfsCycle(Function *f, const DenseMap<Function *, SmallPtrSet<Function *, 4>> &graph,
              DenseMap<Function *, int> &color, std::string &found) {
    color[f] = 1;
    auto it = graph.find(f);
    if (it != graph.end()) {
        for (Function *c : it->second) {
            int cc = color.lookup(c);
            if (cc == 1) {
                found = (f->getName() + " -> " + c->getName()).str();
                return true;
            }
            if (cc == 0 && dfsCycle(c, graph, color, found))
                return true;
        }
    }
    color[f] = 2;
    return false;
}

/// Reference: nx.simple_cycles(function_depgraph) (__init__.py:115-117).
bool detectRecursion(Module &M, std::string &cycle) {
    DenseMap<Function *, SmallPtrSet<Function *, 4>> graph;
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        for (BasicBlock &BB : F)
            for (Instruction &I : BB)
                if (auto *CB = dyn_cast<CallBase>(&I))
                    if (Function *callee = CB->getCalledFunction())
                        if (!callee->isDeclaration())
                            graph[&F].insert(callee);
    }
    DenseMap<Function *, int> color;
    for (auto &entry : graph) {
        Function *f = entry.first;
        if (color.lookup(f) == 0 && dfsCycle(f, graph, color, cycle))
            return true;
    }
    return false;
}

} // namespace

std::vector<IsolatedCall> collectIsolatedCalls(Function &F) {
    DenseMap<unsigned, IsolatedCall> byGid;
    for (BasicBlock &BB : F) {
        for (Instruction &I : BB) {
            if (MDNode *md = I.getMetadata(kSchematicCallEntryMD)) {
                unsigned gid = getGroupId(md);
                auto *CI = cast<CallInst>(&I);
                byGid[gid].entry = &BB;
                byGid[gid].call = CI;
                byGid[gid].callee = CI->getCalledFunction();
            }
            if (MDNode *md = I.getMetadata(kSchematicCallExitMD)) {
                byGid[getGroupId(md)].exit = &BB;
            }
        }
    }
    std::vector<IsolatedCall> out;
    out.reserve(byGid.size());
    for (auto &kv : byGid) {
        IsolatedCall ic = kv.second;
        if (ic.entry && !ic.exit)
            ic.exit = ic.entry->getSingleSuccessor();
        // Drop half-marked groups. Inlining the call deletes it along with its
        // call_entry marker while the exit block's call_exit marker survives,
        // leaving a group with no entry/callee. Callers must see complete call
        // sites only, so such leftovers are ignored rather than folded.
        if (!ic.entry || !ic.exit || !ic.callee)
            continue;
        out.push_back(ic);
    }
    return out;
}

PreservedAnalyses CallIsolationPass::run(Module &M, ModuleAnalysisManager &) {
    std::string cycle;
    if (detectRecursion(M, cycle))
        report_fatal_error(Twine("SCHEMATIC: recursion is unsupported (cycle: ") + cycle + ")");

    unsigned gid = 0;
    bool changed = false;
    for (Function &F : M) {
        if (F.isDeclaration())
            continue;
        if (isBenchmarkInfrastructureFunction(F.getName()))
            continue;
        if (isolateInFunction(F, gid) > 0)
            changed = true;
    }
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace checkpoint
