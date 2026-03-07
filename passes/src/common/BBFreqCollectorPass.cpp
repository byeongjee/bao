#include "common/BBFreqCollectorPass.h"
#include "common/BBNaming.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace llvm;

namespace checkpoint {

// ---------------------------------------------------------------------------
// Helpers (same pattern as TraceCollectorPass)
// ---------------------------------------------------------------------------

static Constant *createGlobalString(Module &M, StringRef str, const Twine &name) {
    Constant *strConst = ConstantDataArray::getString(M.getContext(), str);
    auto *GV = new GlobalVariable(M, strConst->getType(), /*isConstant=*/true,
                                  GlobalValue::PrivateLinkage, strConst, name);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return ConstantExpr::getInBoundsGetElementPtr(
        strConst->getType(), GV,
        ArrayRef<Constant *>{ConstantInt::get(Type::getInt32Ty(M.getContext()), 0),
                             ConstantInt::get(Type::getInt32Ty(M.getContext()), 0)});
}

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

// ---------------------------------------------------------------------------
// BBFreqCollectorPass implementation
// ---------------------------------------------------------------------------

PreservedAnalyses BBFreqCollectorPass::run(Function &F, FunctionAnalysisManager &AM) {
    if (F.isDeclaration())
        return PreservedAnalyses::all();

    // Skip our own runtime functions
    if (F.getName().starts_with("__bb_freq_"))
        return PreservedAnalyses::all();

    Module &M = *F.getParent();
    LLVMContext &Ctx = M.getContext();

    // Ensure all BBs have stable names
    ensureBBNames(F);

    // -------------------------------------------------------------------
    // 1. Assign sequential BB indices and collect names
    // -------------------------------------------------------------------
    std::vector<std::string> bbNames;
    unsigned idx = 0;
    for (BasicBlock &BB : F) {
        bbNames.push_back(BB.getName().str());
        idx++;
    }
    unsigned bbCount = idx;

    if (bbCount == 0)
        return PreservedAnalyses::all();

    // -------------------------------------------------------------------
    // 2. Create per-function global counter array (i64[bbCount], zeroinit)
    // -------------------------------------------------------------------
    Type *I64Ty = Type::getInt64Ty(Ctx);
    ArrayType *counterArrTy = ArrayType::get(I64Ty, bbCount);
    auto *counterGV = new GlobalVariable(
        M, counterArrTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
        ConstantAggregateZero::get(counterArrTy), "__bb_freq_counters_" + F.getName());

    // -------------------------------------------------------------------
    // 3. Create per-function metadata: function name + BB name array
    // -------------------------------------------------------------------
    std::string metaPrefix = "__bb_freq_meta_" + F.getName().str();
    Constant *funcNameStr = createGlobalString(M, F.getName(), metaPrefix + ".name");
    Constant *bbNamesArr = createStringArray(M, bbNames, metaPrefix + ".bb_names");

    // -------------------------------------------------------------------
    // 4. Declare runtime function prototypes
    // -------------------------------------------------------------------
    Type *VoidTy = Type::getVoidTy(Ctx);
    Type *I32Ty = Type::getInt32Ty(Ctx);
    Type *PtrTy = PointerType::getUnqual(Ctx);

    // void __bb_freq_register(const char *func_name, const char **bb_names,
    //                         int bb_count, long long *counters)
    FunctionCallee registerFn =
        M.getOrInsertFunction("__bb_freq_register", VoidTy, PtrTy, PtrTy, I32Ty, PtrTy);

    // void __bb_freq_dump(void)
    FunctionCallee dumpFn = M.getOrInsertFunction("__bb_freq_dump", VoidTy);

    // -------------------------------------------------------------------
    // 5. At function entry: insert __bb_freq_register call
    // -------------------------------------------------------------------
    BasicBlock &entryBB = F.getEntryBlock();
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
    entryBuilder.CreateCall(registerFn,
                            {funcNameStr, bbNamesArr, ConstantInt::get(I32Ty, bbCount), counterGV});

    // -------------------------------------------------------------------
    // 6. At each BB entry (after PHIs): increment counter[bb_idx]
    //    via load/add/store (no function call overhead)
    // -------------------------------------------------------------------
    idx = 0;
    for (BasicBlock &BB : F) {
        BasicBlock::iterator insertIt = BB.getFirstNonPHIIt();
        if (insertIt == BB.end()) {
            idx++;
            continue;
        }

        // In the entry block, insert after the __bb_freq_register call
        Instruction *insertPt = &*insertIt;
        if (&BB == &entryBB) {
            // Find the register call we just inserted
            for (Instruction &I : entryBB) {
                if (auto *CI = dyn_cast<CallInst>(&I)) {
                    if (CI->getCalledFunction() &&
                        CI->getCalledFunction()->getName() == "__bb_freq_register") {
                        insertPt = CI->getNextNode();
                        break;
                    }
                }
            }
        }

        IRBuilder<> builder(insertPt);
        // GEP into counter array: &counters[idx]
        Value *counterPtr = builder.CreateConstInBoundsGEP2_32(counterArrTy, counterGV, 0, idx);
        // Load current count
        Value *count = builder.CreateLoad(I64Ty, counterPtr);
        // Increment
        Value *inc = builder.CreateAdd(count, ConstantInt::get(I64Ty, 1));
        // Store back
        builder.CreateStore(inc, counterPtr);

        idx++;
    }

    // -------------------------------------------------------------------
    // 7. At each return in main(): insert __bb_freq_dump call
    // -------------------------------------------------------------------
    if (F.getName() == "main") {
        for (BasicBlock &BB : F) {
            Instruction *term = BB.getTerminator();
            if (isa<ReturnInst>(term)) {
                IRBuilder<> builder(term);
                builder.CreateCall(dumpFn);
            }
        }
    }

    errs() << "BBFreqCollectorPass: instrumented " << F.getName() << " (" << bbCount << " BBs)\n";

    return PreservedAnalyses::none();
}

} // namespace checkpoint
