#include "milp/StripMiningMetadata.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"

namespace checkpoint {

namespace {

constexpr llvm::StringLiteral kNoSummaryTag = "checkpoint.loop.no_summary";
constexpr llvm::StringLiteral kStripMiningKindTag = "checkpoint.loop.stripmine.kind";
constexpr llvm::StringLiteral kStripMiningRoleTag = "checkpoint.loop.stripmine.role";
constexpr llvm::StringLiteral kStripMiningOriginTag = "checkpoint.loop.stripmine.origin";
constexpr llvm::StringLiteral kStripMiningOrigTripCountTag =
    "checkpoint.loop.stripmine.orig_tripcount";
constexpr llvm::StringLiteral kStripMiningKRoleTag = "checkpoint.stripmine.k";

static bool isLoopMetadataTag(const llvm::MDNode *Op, llvm::StringRef TagName) {
    if (!Op || Op->getNumOperands() < 1)
        return false;
    auto *Tag = llvm::dyn_cast<llvm::MDString>(Op->getOperand(0));
    return Tag && Tag->getString() == TagName;
}

static const llvm::MDNode *findLoopMetadataTag(const llvm::Loop *L, llvm::StringRef TagName) {
    llvm::MDNode *LoopID = L ? L->getLoopID() : nullptr;
    if (!LoopID)
        return nullptr;

    for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
        auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
        if (isLoopMetadataTag(Op, TagName))
            return Op;
    }
    return nullptr;
}

static void removeLoopMetadataTag(llvm::Loop *L, llvm::StringRef TagName) {
    llvm::MDNode *LoopID = L ? L->getLoopID() : nullptr;
    if (!LoopID)
        return;

    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::SmallVector<llvm::Metadata *, 8> MDs;
    MDs.push_back(nullptr);

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

static void setLoopMetadataTag(llvm::Loop *L, llvm::StringRef TagName,
                               llvm::ArrayRef<llvm::Metadata *> ExtraOperands) {
    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::MDNode *LoopID = L->getLoopID();

    llvm::SmallVector<llvm::Metadata *, 8> MDs;
    MDs.push_back(nullptr);

    if (LoopID) {
        for (unsigned i = 1; i < LoopID->getNumOperands(); i++) {
            auto *Op = llvm::dyn_cast<llvm::MDNode>(LoopID->getOperand(i));
            if (isLoopMetadataTag(Op, TagName))
                continue;
            MDs.push_back(LoopID->getOperand(i));
        }
    }

    llvm::SmallVector<llvm::Metadata *, 4> TagOps;
    TagOps.push_back(llvm::MDString::get(Ctx, TagName));
    TagOps.append(ExtraOperands.begin(), ExtraOperands.end());
    MDs.push_back(llvm::MDNode::get(Ctx, TagOps));

    llvm::MDNode *NewLoopID = llvm::MDNode::getDistinct(Ctx, MDs);
    NewLoopID->replaceOperandWith(0, NewLoopID);
    L->setLoopID(NewLoopID);
}

static std::optional<std::string> getLoopStringMetadata(const llvm::Loop *L,
                                                        llvm::StringRef TagName) {
    auto *Tag = findLoopMetadataTag(L, TagName);
    if (!Tag || Tag->getNumOperands() < 2)
        return std::nullopt;
    auto *Value = llvm::dyn_cast<llvm::MDString>(Tag->getOperand(1));
    if (!Value)
        return std::nullopt;
    return Value->getString().str();
}

static void setLoopStringMetadata(llvm::Loop *L, llvm::StringRef TagName, llvm::StringRef Value) {
    llvm::Metadata *Ops[] = {llvm::MDString::get(L->getHeader()->getContext(), Value)};
    setLoopMetadataTag(L, TagName, Ops);
}

static std::optional<uint64_t> getLoopUIntMetadata(const llvm::Loop *L, llvm::StringRef TagName) {
    auto *Tag = findLoopMetadataTag(L, TagName);
    if (!Tag || Tag->getNumOperands() < 2)
        return std::nullopt;
    auto *Value = llvm::mdconst::dyn_extract<llvm::ConstantInt>(Tag->getOperand(1));
    if (!Value)
        return std::nullopt;
    return Value->getZExtValue();
}

static void setLoopUIntMetadata(llvm::Loop *L, llvm::StringRef TagName, uint64_t Value) {
    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::Metadata *Ops[] = {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), Value))};
    setLoopMetadataTag(L, TagName, Ops);
}

} // namespace

bool hasNoSummaryLoopMetadata(const llvm::Loop *L) {
    return findLoopMetadataTag(L, kNoSummaryTag) != nullptr;
}

void setNoSummaryLoopMetadata(llvm::Loop *L) {
    llvm::LLVMContext &Ctx = L->getHeader()->getContext();
    llvm::Metadata *Ops[] = {
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt1Ty(Ctx), 1))};
    setLoopMetadataTag(L, kNoSummaryTag, Ops);
}

void removeNoSummaryLoopMetadata(llvm::Loop *L) {
    removeLoopMetadataTag(L, kNoSummaryTag);
}

std::optional<std::string> getStripMiningKindMetadata(const llvm::Loop *L) {
    return getLoopStringMetadata(L, kStripMiningKindTag);
}

void setStripMiningKindMetadata(llvm::Loop *L, llvm::StringRef kind) {
    setLoopStringMetadata(L, kStripMiningKindTag, kind);
}

std::optional<std::string> getStripMiningRoleMetadata(const llvm::Loop *L) {
    return getLoopStringMetadata(L, kStripMiningRoleTag);
}

void setStripMiningRoleMetadata(llvm::Loop *L, llvm::StringRef role) {
    setLoopStringMetadata(L, kStripMiningRoleTag, role);
}

std::optional<std::string> getStripMiningOriginMetadata(const llvm::Loop *L) {
    return getLoopStringMetadata(L, kStripMiningOriginTag);
}

void setStripMiningOriginMetadata(llvm::Loop *L, llvm::StringRef origin) {
    setLoopStringMetadata(L, kStripMiningOriginTag, origin);
}

std::optional<uint64_t> getStripMiningOriginalTripCountMetadata(const llvm::Loop *L) {
    return getLoopUIntMetadata(L, kStripMiningOrigTripCountTag);
}

void setStripMiningOriginalTripCountMetadata(llvm::Loop *L, uint64_t tripCount) {
    setLoopUIntMetadata(L, kStripMiningOrigTripCountTag, tripCount);
}

void setStripMiningKRoleMetadata(llvm::Instruction *I, llvm::StringRef role) {
    llvm::LLVMContext &Ctx = I->getContext();
    llvm::Metadata *Ops[] = {llvm::MDString::get(Ctx, role)};
    I->setMetadata(kStripMiningKRoleTag, llvm::MDNode::get(Ctx, Ops));
}

std::optional<std::string> getStripMiningKRoleMetadata(const llvm::Instruction *I) {
    llvm::MDNode *Node = I ? I->getMetadata(kStripMiningKRoleTag) : nullptr;
    if (!Node || Node->getNumOperands() < 1)
        return std::nullopt;
    auto *Role = llvm::dyn_cast<llvm::MDString>(Node->getOperand(0));
    if (!Role)
        return std::nullopt;
    return Role->getString().str();
}

} // namespace checkpoint
