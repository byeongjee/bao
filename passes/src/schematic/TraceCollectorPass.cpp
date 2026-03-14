#include "schematic/TraceCollectorPass.h"
#include "common/BBNaming.h"
#include "common/Logger.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include <string>
#include <vector>

using namespace llvm;

namespace checkpoint {

// ---------------------------------------------------------------------------
// Helpers: create global string / array constants
// ---------------------------------------------------------------------------

/// Create a global constant string and return a pointer to it.
static Constant *createGlobalString(Module &M, StringRef str, const Twine &name) {
    Constant *strConst = ConstantDataArray::getString(M.getContext(), str);
    auto *GV = new GlobalVariable(M, strConst->getType(), /*isConstant=*/true,
                                  GlobalValue::PrivateLinkage, strConst, name);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    // Return i8* pointer via constant GEP
    return ConstantExpr::getInBoundsGetElementPtr(
        strConst->getType(), GV,
        ArrayRef<Constant *>{ConstantInt::get(Type::getInt32Ty(M.getContext()), 0),
                             ConstantInt::get(Type::getInt32Ty(M.getContext()), 0)});
}

/// Create a global constant array of i8* pointers from a vector of strings.
static Constant *createStringArray(Module &M, const std::vector<std::string> &strings,
                                   const Twine &name) {
    LLVMContext &Ctx = M.getContext();
    Type *I8PtrTy = PointerType::getUnqual(Ctx);

    SmallVector<Constant *, 32> ptrs;
    for (unsigned i = 0; i < strings.size(); i++) {
        ptrs.push_back(createGlobalString(M, strings[i], name + ".str." + Twine(i)));
    }

    ArrayType *arrTy = ArrayType::get(I8PtrTy, strings.size());
    Constant *arr = ConstantArray::get(arrTy, ptrs);
    auto *GV =
        new GlobalVariable(M, arrTy, /*isConstant=*/true, GlobalValue::PrivateLinkage, arr, name);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return ConstantExpr::getInBoundsGetElementPtr(
        arrTy, GV,
        ArrayRef<Constant *>{ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                             ConstantInt::get(Type::getInt32Ty(Ctx), 0)});
}

/// Create a global constant array of i32 from a vector of ints.
static Constant *createIntArray(Module &M, const std::vector<int> &values, const Twine &name) {
    LLVMContext &Ctx = M.getContext();
    Type *I32Ty = Type::getInt32Ty(Ctx);

    SmallVector<Constant *, 32> elems;
    for (int v : values) {
        elems.push_back(ConstantInt::get(I32Ty, v));
    }

    ArrayType *arrTy = ArrayType::get(I32Ty, values.size());
    Constant *arr = ConstantArray::get(arrTy, elems);
    auto *GV =
        new GlobalVariable(M, arrTy, /*isConstant=*/true, GlobalValue::PrivateLinkage, arr, name);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return ConstantExpr::getInBoundsGetElementPtr(
        arrTy, GV, ArrayRef<Constant *>{ConstantInt::get(I32Ty, 0), ConstantInt::get(I32Ty, 0)});
}

/// Create a global constant array of i8** (array of string-array pointers).
static Constant *createStringArrayArray(Module &M,
                                        const std::vector<std::vector<std::string>> &arrays,
                                        const Twine &name) {
    LLVMContext &Ctx = M.getContext();
    Type *ElemPtrTy = PointerType::getUnqual(Ctx);

    SmallVector<Constant *, 16> ptrs;
    for (unsigned i = 0; i < arrays.size(); i++) {
        if (arrays[i].empty()) {
            ptrs.push_back(ConstantPointerNull::get(cast<PointerType>(ElemPtrTy)));
        } else {
            ptrs.push_back(createStringArray(M, arrays[i], name + "." + Twine(i)));
        }
    }

    ArrayType *arrTy = ArrayType::get(ElemPtrTy, arrays.size());
    Constant *arr = ConstantArray::get(arrTy, ptrs);
    auto *GV =
        new GlobalVariable(M, arrTy, /*isConstant=*/true, GlobalValue::PrivateLinkage, arr, name);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return ConstantExpr::getInBoundsGetElementPtr(
        arrTy, GV,
        ArrayRef<Constant *>{ConstantInt::get(Type::getInt32Ty(Ctx), 0),
                             ConstantInt::get(Type::getInt32Ty(Ctx), 0)});
}

// ---------------------------------------------------------------------------
// TraceCollectorPass implementation
// ---------------------------------------------------------------------------

PreservedAnalyses TraceCollectorPass::run(Function &F, FunctionAnalysisManager &AM) {
    initLogging();
    // Skip declarations
    if (F.isDeclaration())
        return PreservedAnalyses::all();

    // Skip our own runtime functions
    if (F.getName().starts_with("__trace_"))
        return PreservedAnalyses::all();

    Module &M = *F.getParent();
    LLVMContext &Ctx = M.getContext();

    // Ensure all BBs have names (shared with schematic pass)
    ensureBBNames(F);

    // Get analyses
    auto &LI = AM.getResult<LoopAnalysis>(F);

    // -----------------------------------------------------------------------
    // 1. Assign sequential BB indices
    // -----------------------------------------------------------------------
    DenseMap<BasicBlock *, unsigned> bbIndex;
    std::vector<std::string> bbNames;
    unsigned idx = 0;
    for (BasicBlock &BB : F) {
        bbIndex[&BB] = idx;
        bbNames.push_back(BB.getName().str());
        idx++;
    }
    unsigned bbCount = idx;

    // -----------------------------------------------------------------------
    // 2. Collect loop metadata
    // -----------------------------------------------------------------------
    struct LoopMeta {
        int loopId;
        Loop *loop;
        BasicBlock *header;
        BasicBlock *preheader;
        BasicBlock *latch;
        unsigned headerIdx;
        std::string headerName;
        std::string latchName;
        unsigned depth;
        std::vector<std::string> memberNames;
        std::vector<std::string> exitingNames;
    };

    // Collect loops innermost-first (for instrumentation order)
    std::vector<LoopMeta> loops;
    {
        auto allLoops = LI.getLoopsInPreorder();
        // Reverse to get innermost first
        int loopId = 0;
        for (auto it = allLoops.rbegin(); it != allLoops.rend(); ++it) {
            Loop *L = *it;
            BasicBlock *header = L->getHeader();
            BasicBlock *preheader = L->getLoopPreheader();
            BasicBlock *latch = L->getLoopLatch();

            if (!preheader || !latch) {
                PLOGW << "TraceCollectorPass: skipping loop without "
                      << "preheader/latch at " << header->getName();
                continue;
            }

            LoopMeta lm;
            lm.loopId = loopId++;
            lm.loop = L;
            lm.header = header;
            lm.preheader = preheader;
            lm.latch = latch;
            lm.headerIdx = bbIndex[header];
            lm.headerName = header->getName().str();
            lm.latchName = latch->getName().str();
            lm.depth = L->getLoopDepth();

            for (BasicBlock *BB : L->blocks())
                lm.memberNames.push_back(BB->getName().str());

            SmallVector<BasicBlock *, 4> exitingBlocks;
            L->getExitingBlocks(exitingBlocks);
            for (BasicBlock *EB : exitingBlocks)
                lm.exitingNames.push_back(EB->getName().str());

            loops.push_back(std::move(lm));
        }
    }
    unsigned loopCount = loops.size();

    // -----------------------------------------------------------------------
    // 3. Declare runtime function prototypes
    // -----------------------------------------------------------------------
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *I32Ty = Type::getInt32Ty(Ctx);
    Type *PtrTy = PointerType::getUnqual(Ctx);

    FunctionCallee funcEnterFn = M.getOrInsertFunction("__trace_func_enter", VoidTy, PtrTy);
    FunctionCallee bbFn = M.getOrInsertFunction("__trace_bb", VoidTy, I32Ty);
    FunctionCallee loopEnterFn = M.getOrInsertFunction("__trace_loop_enter", VoidTy, I32Ty, I32Ty);
    FunctionCallee loopIterEndFn = M.getOrInsertFunction("__trace_loop_iter_end", VoidTy, I32Ty);
    FunctionCallee loopExitFn = M.getOrInsertFunction("__trace_loop_exit", VoidTy, I32Ty);
    FunctionCallee funcExitFn = M.getOrInsertFunction("__trace_func_exit", VoidTy);

    // -----------------------------------------------------------------------
    // 4. Emit per-function metadata as a FuncTraceMeta struct
    // -----------------------------------------------------------------------
    // The C runtime defines:
    //   typedef struct {
    //       const char *func_name;
    //       int bb_count;
    //       const char **bb_names;
    //       int loop_count;
    //       const int *loop_header_bb_idx;
    //       const char **loop_header_names;
    //       const char **loop_latch_names;
    //       const int *loop_depths;
    //       const int *loop_member_counts;
    //       const char ***loop_member_names;
    //       const int *loop_exiting_counts;
    //       const char ***loop_exiting_names;
    //   } FuncTraceMeta;

    std::string funcName = F.getName().str();
    std::string metaPrefix = "__trace_meta_" + funcName;

    Constant *funcNameStr = createGlobalString(M, funcName, metaPrefix + ".name");
    Constant *bbNamesArr = createStringArray(M, bbNames, metaPrefix + ".bb_names");

    // Loop parallel arrays
    std::vector<int> headerIdxVec, depthVec, memberCountVec, exitingCountVec;
    std::vector<std::string> headerNameVec, latchNameVec;
    std::vector<std::vector<std::string>> memberNamesVec, exitingNamesVec;

    for (const auto &lm : loops) {
        headerIdxVec.push_back(lm.headerIdx);
        depthVec.push_back(lm.depth);
        headerNameVec.push_back(lm.headerName);
        latchNameVec.push_back(lm.latchName);
        memberCountVec.push_back(lm.memberNames.size());
        memberNamesVec.push_back(lm.memberNames);
        exitingCountVec.push_back(lm.exitingNames.size());
        exitingNamesVec.push_back(lm.exitingNames);
    }

    // Build the metadata struct type to match FuncTraceMeta
    // {i8*, i32, i8**, i32, i32*, i8**, i8**, i32*, i32*, i8***, i32*, i8***}
    StructType *metaStructTy = StructType::create(
        Ctx, {PtrTy, I32Ty, PtrTy, I32Ty, PtrTy, PtrTy, PtrTy, PtrTy, PtrTy, PtrTy, PtrTy, PtrTy},
        metaPrefix + ".type");

    // Create the struct constant
    SmallVector<Constant *, 12> metaFields;
    metaFields.push_back(funcNameStr);
    metaFields.push_back(ConstantInt::get(I32Ty, bbCount));
    metaFields.push_back(bbNamesArr);
    metaFields.push_back(ConstantInt::get(I32Ty, loopCount));

    if (loopCount > 0) {
        metaFields.push_back(createIntArray(M, headerIdxVec, metaPrefix + ".header_idx"));
        metaFields.push_back(createStringArray(M, headerNameVec, metaPrefix + ".header_names"));
        metaFields.push_back(createStringArray(M, latchNameVec, metaPrefix + ".latch_names"));
        metaFields.push_back(createIntArray(M, depthVec, metaPrefix + ".depths"));
        metaFields.push_back(createIntArray(M, memberCountVec, metaPrefix + ".member_counts"));
        metaFields.push_back(
            createStringArrayArray(M, memberNamesVec, metaPrefix + ".member_names"));
        metaFields.push_back(createIntArray(M, exitingCountVec, metaPrefix + ".exiting_counts"));
        metaFields.push_back(
            createStringArrayArray(M, exitingNamesVec, metaPrefix + ".exiting_names"));
    } else {
        // Null pointers for all loop arrays
        for (int i = 0; i < 8; i++)
            metaFields.push_back(ConstantPointerNull::get(cast<PointerType>(PtrTy)));
    }

    Constant *metaStruct = ConstantStruct::get(metaStructTy, metaFields);
    auto *metaGV = new GlobalVariable(M, metaStructTy, /*isConstant=*/true,
                                      GlobalValue::PrivateLinkage, metaStruct, metaPrefix);

    // -----------------------------------------------------------------------
    // 5. Insert __trace_func_enter at function entry
    // -----------------------------------------------------------------------
    BasicBlock &entryBB = F.getEntryBlock();
    // Find first non-alloca instruction
    Instruction *entryInsertPt = nullptr;
    for (Instruction &I : entryBB) {
        if (!isa<AllocaInst>(&I)) {
            entryInsertPt = &I;
            break;
        }
    }
    if (!entryInsertPt)
        entryInsertPt = entryBB.getTerminator();

    IRBuilder<> entryBuilder(entryInsertPt);
    entryBuilder.CreateCall(funcEnterFn, {metaGV});

    // -----------------------------------------------------------------------
    // 6. Insert __trace_bb at start of each BB (after PHIs)
    // -----------------------------------------------------------------------
    for (BasicBlock &BB : F) {
        BasicBlock::iterator insertIt = BB.getFirstNonPHIIt();
        if (insertIt == BB.end())
            continue;
        Instruction *insertPt = &*insertIt;
        // Skip past allocas in entry block (func_enter is already placed there)
        if (&BB == &entryBB) {
            insertPt = entryInsertPt;
            // Place after the __trace_func_enter call we just inserted
            // Find the call we inserted
            for (Instruction &I : entryBB) {
                if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (CI->getCalledFunction() &&
                        CI->getCalledFunction()->getName() == "__trace_func_enter") {
                        insertPt = CI->getNextNode();
                        break;
                    }
                }
            }
        }
        IRBuilder<> builder(insertPt);
        builder.CreateCall(bbFn, {ConstantInt::get(I32Ty, bbIndex[&BB])});
    }

    // -----------------------------------------------------------------------
    // 7. Insert loop instrumentation (innermost first)
    // -----------------------------------------------------------------------
    for (const auto &lm : loops) {
        Value *loopIdVal = ConstantInt::get(I32Ty, lm.loopId);
        Value *headerIdxVal = ConstantInt::get(I32Ty, lm.headerIdx);

        // 7a. In preheader (before terminator): __trace_loop_enter
        {
            IRBuilder<> builder(lm.preheader->getTerminator());
            builder.CreateCall(loopEnterFn, {loopIdVal, headerIdxVal});
        }

        // 7b. In latch (before terminator): __trace_loop_iter_end
        {
            IRBuilder<> builder(lm.latch->getTerminator());
            builder.CreateCall(loopIterEndFn, {loopIdVal});
        }

        // 7c. For each exit block: insert __trace_loop_exit
        // LoopSimplify guarantees dedicated exit blocks, so no edge splitting needed.
        SmallVector<BasicBlock *, 4> exitBlocks;
        lm.loop->getExitBlocks(exitBlocks);

        // Deduplicate (multiple exiting edges may go to the same exit block)
        SmallPtrSet<BasicBlock *, 4> seen;
        for (BasicBlock *exitBB : exitBlocks) {
            if (!seen.insert(exitBB).second)
                continue;
            IRBuilder<> builder(&*exitBB->getFirstNonPHIIt());
            builder.CreateCall(loopExitFn, {loopIdVal});
        }
    }

    // -----------------------------------------------------------------------
    // 8. Insert __trace_func_exit before each ret
    // -----------------------------------------------------------------------
    for (BasicBlock &BB : F) {
        Instruction *term = BB.getTerminator();
        if (isa<ReturnInst>(term)) {
            IRBuilder<> builder(term);
            builder.CreateCall(funcExitFn);
        }
    }

    PLOGI << "TraceCollectorPass: instrumented " << funcName << " (" << bbCount << " BBs, "
          << loopCount << " loops)";

    return PreservedAnalyses::none();
}

} // namespace checkpoint
