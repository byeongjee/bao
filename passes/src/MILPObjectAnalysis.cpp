#include "MILPObjectAnalysis.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"

namespace checkpoint {

MILPObjectAnalysis::MILPObjectAnalysis(llvm::Function &F)
    : F_(F), M_(*F.getParent()) {}

MILPObjectAnalysisResult MILPObjectAnalysis::analyze() {
    MILPObjectAnalysisResult result;

    for (llvm::GlobalVariable &GV : M_.globals()) {
        if (shouldSkipGlobal(GV)) {
            continue;
        }

        MILPTrackedObject tracked;
        tracked.value = &GV;
        tracked.name = getObjectName(&GV);
        tracked.sizeBytes = getObjectSizeBytes(&GV);
        tracked.isGlobal = true;

        bool aliasAmbiguous = false;
        tracked.pointerEscaped = valueEscapes(&GV, aliasAmbiguous);
        tracked.aliasAmbiguous = aliasAmbiguous;

        if (tracked.pointerEscaped || tracked.aliasAmbiguous) {
            tracked.reason = "Pointer escaped or aliasing is ambiguous";
            result.forcedNVMObjects.push_back(std::move(tracked));
        } else {
            tracked.reason = "Eligible VM candidate in phase-1";
            result.vmCandidateObjects.push_back(std::move(tracked));
        }
    }

    // Keep allocas out of placement optimization in phase-1.
    for (llvm::BasicBlock &BB : F_) {
        for (llvm::Instruction &I : BB) {
            auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
            if (!alloca) {
                continue;
            }
            MILPTrackedObject tracked;
            tracked.value = alloca;
            tracked.name = getObjectName(alloca);
            tracked.sizeBytes = getObjectSizeBytes(alloca);
            tracked.isGlobal = false;
            tracked.pointerEscaped = true;
            tracked.aliasAmbiguous = true;
            tracked.reason = "Alloca excluded from phase-1 placement optimization";
            result.forcedNVMObjects.push_back(std::move(tracked));
        }
    }

    return result;
}

bool MILPObjectAnalysis::valueEscapes(llvm::Value *ptrValue, bool &aliasAmbiguous) {
    llvm::SmallPtrSet<llvm::Value*, 32> visited;
    return valueEscapesRecursive(ptrValue, visited, aliasAmbiguous);
}

bool MILPObjectAnalysis::valueEscapesRecursive(llvm::Value *ptrValue,
                                               llvm::SmallPtrSetImpl<llvm::Value*> &visited,
                                               bool &aliasAmbiguous) {
    if (!visited.insert(ptrValue).second) {
        return false;
    }

    for (llvm::User *user : ptrValue->users()) {
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(user)) {
            if (load->getPointerOperand() == ptrValue) {
                continue;
            }
            aliasAmbiguous = true;
            return true;
        }

        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user)) {
            if (store->getPointerOperand() == ptrValue) {
                continue;
            }
            if (store->getValueOperand() == ptrValue) {
                return true;
            }
            aliasAmbiguous = true;
            return true;
        }

        if (llvm::isa<llvm::GetElementPtrInst>(user) ||
            llvm::isa<llvm::BitCastInst>(user) ||
            llvm::isa<llvm::AddrSpaceCastInst>(user)) {
            if (valueEscapesRecursive(llvm::cast<llvm::Value>(user), visited, aliasAmbiguous)) {
                return true;
            }
            continue;
        }

        if (llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::SelectInst>(user)) {
            aliasAmbiguous = true;
            if (valueEscapesRecursive(llvm::cast<llvm::Value>(user), visited, aliasAmbiguous)) {
                return true;
            }
            continue;
        }

        if (llvm::isa<llvm::PtrToIntInst>(user) || llvm::isa<llvm::IntToPtrInst>(user)) {
            return true;
        }

        if (auto *call = llvm::dyn_cast<llvm::CallBase>(user)) {
            if (isNonEscapingIntrinsic(*call)) {
                continue;
            }
            aliasAmbiguous = true;
            return true;
        }

        if (llvm::isa<llvm::ReturnInst>(user)) {
            return true;
        }

        // Conservatively treat unknown pointer uses as escaping.
        aliasAmbiguous = true;
        return true;
    }

    return false;
}

bool MILPObjectAnalysis::isNonEscapingIntrinsic(const llvm::CallBase &call) const {
    auto *intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&call);
    if (!intrinsic) {
        return false;
    }

    switch (intrinsic->getIntrinsicID()) {
    case llvm::Intrinsic::dbg_declare:
    case llvm::Intrinsic::dbg_value:
    case llvm::Intrinsic::dbg_assign:
    case llvm::Intrinsic::lifetime_start:
    case llvm::Intrinsic::lifetime_end:
        return true;
    default:
        return false;
    }
}

bool MILPObjectAnalysis::shouldSkipGlobal(const llvm::GlobalVariable &GV) const {
    if (GV.isDeclaration() || GV.isConstant()) {
        return true;
    }
    if (GV.getName().starts_with("llvm.")) {
        return true;
    }
    if (GV.getName().starts_with("__nvm_") || GV.getName().starts_with("__milp_")) {
        return true;
    }
    return false;
}

uint64_t MILPObjectAnalysis::getObjectSizeBytes(llvm::Value *value) const {
    const llvm::DataLayout &dataLayout = M_.getDataLayout();
    llvm::Type *type = nullptr;

    if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(value)) {
        type = GV->getValueType();
    } else if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(value)) {
        type = alloca->getAllocatedType();
    }

    if (!type || !type->isSized()) {
        return 0;
    }

    llvm::TypeSize size = dataLayout.getTypeStoreSize(type);
    if (size.isScalable()) {
        return 0;
    }
    return size.getFixedValue();
}

std::string MILPObjectAnalysis::getObjectName(const llvm::Value *value) const {
    if (!value->hasName()) {
        return "<unnamed>";
    }
    return value->getName().str();
}

} // namespace checkpoint
