#include "common/LoopTripCount.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

namespace checkpoint {

std::map<const llvm::Loop*, unsigned>
LoopTripCount::extractBounds(llvm::Function &F, llvm::LoopInfo &LI) {
    std::map<const llvm::Loop*, unsigned> bounds;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            auto *CI = llvm::dyn_cast<llvm::CallInst>(&I);
            if (!CI)
                continue;

            llvm::Function *Callee = CI->getCalledFunction();
            if (!Callee || Callee->getName() != "__loop_tripcount")
                continue;

            // Extract the constant argument
            if (CI->arg_size() < 1)
                continue;

            auto *Arg = llvm::dyn_cast<llvm::ConstantInt>(CI->getArgOperand(0));
            if (!Arg) {
                llvm::errs() << "Warning: __loop_tripcount argument is not a "
                                "constant in block "
                             << BB.getName() << "\n";
                continue;
            }

            unsigned tripCount = Arg->getZExtValue();

            // Find the containing loop
            llvm::Loop *L = LI.getLoopFor(&BB);
            if (!L) {
                llvm::errs() << "Warning: __loop_tripcount call not inside a "
                                "loop in block "
                             << BB.getName() << "\n";
                continue;
            }

            // Store the bound (if multiple annotations for same loop, use max)
            auto it = bounds.find(L);
            if (it == bounds.end() || tripCount > it->second) {
                bounds[L] = tripCount;
            }
        }
    }

    return bounds;
}

} // namespace checkpoint
