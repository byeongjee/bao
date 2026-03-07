#include "common/LoopTripCount.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"

namespace checkpoint {

namespace {

constexpr llvm::StringLiteral kTripCountTag = "llvm.loop.tripcount.upper";
constexpr llvm::StringLiteral kStripMinedTag = "checkpoint.loop.stripmined";

static bool isLoopMetadataTag(const llvm::MDNode *Op, llvm::StringRef TagName) {
    if (!Op || Op->getNumOperands() < 1)
        return false;
    auto *Tag = llvm::dyn_cast<llvm::MDString>(Op->getOperand(0));
    return Tag && Tag->getString() == TagName;
}

static void removeLoopMetadataTag(llvm::Loop *L, llvm::StringRef TagName) {
    llvm::MDNode *LoopID = L->getLoopID();
    if (!LoopID)
        return;

    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::SmallVector<llvm::Metadata *, 4> MDs;
    MDs.push_back(nullptr); // self-ref placeholder

    bool found = false;
    for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
        auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
        if (isLoopMetadataTag(Op, TagName)) {
            found = true;
            continue;
        }
        MDs.push_back(LoopID->getOperand(i));
    }

    if (!found)
        return;

    if (MDs.size() == 1) {
        L->setLoopID(nullptr);
        return;
    }

    llvm::MDNode *NewLoopID = llvm::MDNode::getDistinct(Ctx, MDs);
    NewLoopID->replaceOperandWith(0, NewLoopID);
    L->setLoopID(NewLoopID);
}

} // namespace

std::optional<uint64_t> getMarkerTripCount(const llvm::Loop *L) {
    llvm::MDNode *LoopID = L->getLoopID();
    if (!LoopID)
        return std::nullopt;
    for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
        auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
        if (!Op || Op->getNumOperands() < 2)
            continue;
        if (isLoopMetadataTag(Op, kTripCountTag)) {
            auto *Val = llvm::mdconst::dyn_extract<llvm::ConstantInt>(Op->getOperand(1));
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
                if (Tag && Tag->getString() == kTripCountTag)
                    continue;
            }
            MDs.push_back(LoopID->getOperand(i));
        }
    }

    // Append the new tripcount entry.
    llvm::Metadata *TCOps[] = {llvm::MDString::get(Ctx, kTripCountTag),
                               llvm::ConstantAsMetadata::get(
                                   llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), tripCount))};
    MDs.push_back(llvm::MDNode::get(Ctx, TCOps));

    llvm::MDNode *NewLoopID = llvm::MDNode::getDistinct(Ctx, MDs);
    NewLoopID->replaceOperandWith(0, NewLoopID);
    L->setLoopID(NewLoopID);
}

void removeLoopTripCountMetadata(llvm::Loop *L) {
    removeLoopMetadataTag(L, kTripCountTag);
}

bool hasStripMinedLoopMetadata(const llvm::Loop *L) {
    llvm::MDNode *LoopID = L->getLoopID();
    if (!LoopID)
        return false;

    for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
        auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
        if (isLoopMetadataTag(Op, kStripMinedTag)) {
            return true;
        }
    }
    return false;
}

void setStripMinedLoopMetadata(llvm::Loop *L) {
    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::MDNode *LoopID = L->getLoopID();

    llvm::SmallVector<llvm::Metadata *, 4> MDs;
    MDs.push_back(nullptr);

    if (LoopID) {
        for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
            auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
            if (isLoopMetadataTag(Op, kStripMinedTag))
                continue;
            MDs.push_back(LoopID->getOperand(i));
        }
    }

    llvm::Metadata *StripMinedOps[] = {
        llvm::MDString::get(Ctx, kStripMinedTag),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt1Ty(Ctx), 1))};
    MDs.push_back(llvm::MDNode::get(Ctx, StripMinedOps));

    llvm::MDNode *NewLoopID = llvm::MDNode::getDistinct(Ctx, MDs);
    NewLoopID->replaceOperandWith(0, NewLoopID);
    L->setLoopID(NewLoopID);
}

void removeStripMinedLoopMetadata(llvm::Loop *L) {
    removeLoopMetadataTag(L, kStripMinedTag);
}

} // namespace checkpoint
