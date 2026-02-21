#include "common/LoopTripCount.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"

namespace checkpoint {

std::optional<uint64_t> getMarkerTripCount(const llvm::Loop *L) {
    llvm::MDNode *LoopID = L->getLoopID();
    if (!LoopID)
        return std::nullopt;
    for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
        auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
        if (!Op || Op->getNumOperands() < 2)
            continue;
        auto *Tag = llvm::dyn_cast<llvm::MDString>(Op->getOperand(0));
        if (Tag && Tag->getString() == "llvm.loop.tripcount.upper") {
            auto *Val = llvm::mdconst::dyn_extract<llvm::ConstantInt>(
                Op->getOperand(1));
            if (Val)
                return Val->getZExtValue();
        }
    }
    return std::nullopt;
}

void setLoopTripCountMetadata(llvm::Loop *L, uint64_t tripCount) {
    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::MDNode *LoopID = L->getLoopID();

    llvm::SmallVector<llvm::Metadata *, 4> MDs;
    // First operand is self-reference (placeholder, filled after creation).
    MDs.push_back(nullptr);

    // Preserve existing entries, skipping any previous tripcount.
    if (LoopID) {
        for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
            auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
            if (Op && Op->getNumOperands() >= 1) {
                auto *Tag = llvm::dyn_cast<llvm::MDString>(Op->getOperand(0));
                if (Tag && Tag->getString() == "llvm.loop.tripcount.upper")
                    continue;
            }
            MDs.push_back(LoopID->getOperand(i));
        }
    }

    // Append the new tripcount entry.
    llvm::Metadata *TCOps[] = {
        llvm::MDString::get(Ctx, "llvm.loop.tripcount.upper"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), tripCount))};
    MDs.push_back(llvm::MDNode::get(Ctx, TCOps));

    llvm::MDNode *NewLoopID = llvm::MDNode::getDistinct(Ctx, MDs);
    NewLoopID->replaceOperandWith(0, NewLoopID);
    L->setLoopID(NewLoopID);
}

void removeLoopTripCountMetadata(llvm::Loop *L) {
    llvm::MDNode *LoopID = L->getLoopID();
    if (!LoopID)
        return;

    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::SmallVector<llvm::Metadata *, 4> MDs;
    MDs.push_back(nullptr); // self-ref placeholder

    bool found = false;
    for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
        auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
        if (Op && Op->getNumOperands() >= 1) {
            auto *Tag = llvm::dyn_cast<llvm::MDString>(Op->getOperand(0));
            if (Tag && Tag->getString() == "llvm.loop.tripcount.upper") {
                found = true;
                continue;
            }
        }
        MDs.push_back(LoopID->getOperand(i));
    }

    if (!found)
        return;

    if (MDs.size() == 1) {
        // Only self-reference left — remove the loop ID entirely.
        L->setLoopID(nullptr);
        return;
    }

    llvm::MDNode *NewLoopID = llvm::MDNode::getDistinct(Ctx, MDs);
    NewLoopID->replaceOperandWith(0, NewLoopID);
    L->setLoopID(NewLoopID);
}

} // namespace checkpoint
