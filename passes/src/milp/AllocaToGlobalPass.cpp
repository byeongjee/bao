#include "milp/AllocaToGlobalPass.h"

#include "common/Logger.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"

namespace checkpoint {

using namespace llvm;

namespace {

/// Type of the global standing in for *AI*, re-adding the dimension that a
/// non-unit `alloca T, i32 N` keeps in its array size rather than its type.
Type *globalTypeFor(const AllocaInst *AI) {
    Type *elemTy = AI->getAllocatedType();
    uint64_t count = cast<ConstantInt>(AI->getArraySize())->getZExtValue();
    return count == 1 ? elemTy : ArrayType::get(elemTy, count);
}

/// Lifetime markers require an alloca operand, so they cannot survive the
/// rewrite.
void eraseLifetimeMarkers(AllocaInst *AI) {
    SmallVector<IntrinsicInst *, 4> markers;
    for (User *U : AI->users()) {
        auto *II = dyn_cast<IntrinsicInst>(U);
        if (!II)
            continue;
        if (II->getIntrinsicID() == Intrinsic::lifetime_start ||
            II->getIntrinsicID() == Intrinsic::lifetime_end)
            markers.push_back(II);
    }
    for (IntrinsicInst *II : markers)
        II->eraseFromParent();
}

} // namespace

PreservedAnalyses AllocaToGlobalPass::run(Function &F, FunctionAnalysisManager &) {
    if (F.getName() != "main")
        return PreservedAnalyses::all();

    SmallVector<AllocaInst *, 8> targets;
    for (Instruction &I : F.getEntryBlock()) {
        auto *AI = dyn_cast<AllocaInst>(&I);
        if (AI && AI->isStaticAlloca())
            targets.push_back(AI);
    }

    if (targets.empty())
        return PreservedAnalyses::all();

    Module &M = *F.getParent();
    // .fram is an MSP430 linker section; host builds (BB frequency
    // collection) reject it as a mach-o section specifier.
    bool useFramSection = M.getTargetTriple().getArch() == Triple::msp430;
    for (AllocaInst *AI : targets) {
        Type *globalTy = globalTypeFor(AI);
        std::string name =
            "__local_" + F.getName().str() + "." + (AI->hasName() ? AI->getName().str() : "alloca");

        // Always fresh: two unnamed allocas share a name, and reusing the
        // global would collapse them onto one object.
        auto *GV =
            new GlobalVariable(M, globalTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
                               Constant::getNullValue(globalTy), name);
        if (useFramSection)
            GV->setSection(".fram");
        GV->setAlignment(AI->getAlign());

        eraseLifetimeMarkers(AI);
        AI->replaceAllUsesWith(GV);
        AI->eraseFromParent();

        PLOGI << "AllocaToGlobalPass: " << F.getName() << " rewrote alloca to " << GV->getName();
    }

    return PreservedAnalyses::none();
}

} // namespace checkpoint
