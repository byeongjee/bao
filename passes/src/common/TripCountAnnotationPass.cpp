#include "common/TripCountAnnotationPass.h"
#include "common/LoopTripCount.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

namespace checkpoint {

llvm::PreservedAnalyses TripCountAnnotationPass::run(llvm::Function &F,
                                                     llvm::FunctionAnalysisManager &AM) {
    if (F.isDeclaration())
        return llvm::PreservedAnalyses::all();

    auto &LI = AM.getResult<llvm::LoopAnalysis>(F);

    // Step 1: Collect all __loop_tripcount calls and map them to loops.
    std::map<llvm::Loop *, uint64_t> loopTripCounts;
    llvm::SmallVector<llvm::CallInst *, 8> toErase;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            auto *CI = llvm::dyn_cast<llvm::CallInst>(&I);
            if (!CI)
                continue;

            llvm::Function *Callee = CI->getCalledFunction();
            if (!Callee || Callee->getName() != "__loop_tripcount")
                continue;

            toErase.push_back(CI);

            if (CI->arg_size() < 1)
                continue;

            auto *Arg = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(0));
            if (!Arg) {
                llvm::errs() << "Warning: __loop_tripcount argument is not a "
                                "constant in block "
                             << BB.getName() << "\n";
                continue;
            }

            uint64_t tripCount = Arg->getZExtValue();

            llvm::Loop *L = LI.getLoopFor(&BB);
            if (!L) {
                llvm::errs() << "Warning: __loop_tripcount call not inside a "
                                "loop in block "
                             << BB.getName() << "\n";
                continue;
            }

            // If multiple calls map to the same loop, take the max.
            auto it = loopTripCounts.find(L);
            if (it == loopTripCounts.end() || tripCount > it->second)
                loopTripCounts[L] = tripCount;
        }
    }

    if (toErase.empty())
        return llvm::PreservedAnalyses::all();

    // Step 2: Set !llvm.loop metadata on each loop.
    for (auto &[L, tc] : loopTripCounts) {
        setLoopTripCountMetadata(L, tc);
    }

    // Step 3: Erase all __loop_tripcount call instructions.
    llvm::Function *MarkerFn = nullptr;
    for (llvm::CallInst *CI : toErase) {
        if (!MarkerFn)
            MarkerFn = CI->getCalledFunction();
        CI->eraseFromParent();
    }

    // Step 4: If the marker function declaration has no remaining uses,
    // remove it from the module.
    if (MarkerFn && MarkerFn->use_empty())
        MarkerFn->eraseFromParent();

    // Step 5: Verify that every loop in the function now has a tripcount
    // annotation.  Missing annotations produce hard-to-diagnose skip reasons
    // downstream, so we catch them here.
    bool missingAnnotation = false;
    for (llvm::Loop *L : LI) {
        llvm::SmallVector<llvm::Loop *, 8> worklist;
        worklist.push_back(L);
        while (!worklist.empty()) {
            llvm::Loop *Current = worklist.pop_back_val();
            if (!getMarkerTripCount(Current)) {
                llvm::errs() << "Error: loop in " << F.getName() << " at block '"
                             << Current->getHeader()->getName()
                             << "' is missing a __loop_tripcount annotation\n";
                missingAnnotation = true;
            }
            for (llvm::Loop *Sub : *Current)
                worklist.push_back(Sub);
        }
    }

    if (missingAnnotation) {
        llvm::report_fatal_error("TripCountAnnotationPass: not all loops are annotated with "
                                 "__loop_tripcount(); add annotations to the source or the pass "
                                 "will not be able to determine trip counts");
    }

    return llvm::PreservedAnalyses::none();
}

} // namespace checkpoint
