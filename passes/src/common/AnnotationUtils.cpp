#include "common/AnnotationUtils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

namespace checkpoint {

namespace {

static llvm::GlobalVariable *getAnnotatedGlobalFromConst(llvm::Constant *C) {
    if (!C)
        return nullptr;
    C = C->stripPointerCasts();
    return llvm::dyn_cast<llvm::GlobalVariable>(C);
}

} // namespace

std::string extractAnnotationString(llvm::Constant *AnnoOp) {
    if (!AnnoOp)
        return "";

    llvm::GlobalVariable *StrGV = nullptr;

    if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(AnnoOp)) {
        if (CE->getOpcode() == llvm::Instruction::GetElementPtr) {
            if (auto *Base = llvm::dyn_cast<llvm::Constant>(CE->getOperand(0))) {
                StrGV = llvm::dyn_cast<llvm::GlobalVariable>(Base->stripPointerCasts());
            }
        }
    } else {
        StrGV = llvm::dyn_cast<llvm::GlobalVariable>(AnnoOp->stripPointerCasts());
    }

    if (!StrGV || !StrGV->hasInitializer())
        return "";

    if (auto *CDS = llvm::dyn_cast<llvm::ConstantDataSequential>(StrGV->getInitializer())) {
        if (CDS->isCString())
            return CDS->getAsCString().str();
    }
    return "";
}

bool isMilpCandidateAnnotated(llvm::GlobalVariable *GV, llvm::Module *M) {
    if (!M)
        return false;

    llvm::GlobalVariable *AnnoGV = M->getNamedGlobal("llvm.global.annotations");
    if (!AnnoGV || !AnnoGV->hasInitializer())
        return false;

    auto *CA = llvm::dyn_cast<llvm::ConstantArray>(AnnoGV->getInitializer());
    if (!CA)
        return false;

    for (unsigned i = 0; i < CA->getNumOperands(); ++i) {
        auto *CS = llvm::dyn_cast<llvm::ConstantStruct>(CA->getOperand(i));
        if (!CS || CS->getNumOperands() < 2)
            continue;

        llvm::GlobalVariable *Target =
            getAnnotatedGlobalFromConst(llvm::dyn_cast<llvm::Constant>(CS->getOperand(0)));
        if (Target != GV)
            continue;

        std::string Anno =
            extractAnnotationString(llvm::dyn_cast<llvm::Constant>(CS->getOperand(1)));
        if (Anno == "milp_candidate")
            return true;
    }

    return false;
}

} // namespace checkpoint
