#pragma once

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"

#include <optional>
#include <string>

namespace checkpoint {

bool hasNoSummaryLoopMetadata(const llvm::Loop *L);
void setNoSummaryLoopMetadata(llvm::Loop *L);
void removeNoSummaryLoopMetadata(llvm::Loop *L);

std::optional<std::string> getStripMiningKindMetadata(const llvm::Loop *L);
void setStripMiningKindMetadata(llvm::Loop *L, llvm::StringRef kind);

std::optional<std::string> getStripMiningRoleMetadata(const llvm::Loop *L);
void setStripMiningRoleMetadata(llvm::Loop *L, llvm::StringRef role);

std::optional<std::string> getStripMiningOriginMetadata(const llvm::Loop *L);
void setStripMiningOriginMetadata(llvm::Loop *L, llvm::StringRef origin);

std::optional<uint64_t> getStripMiningOriginalTripCountMetadata(const llvm::Loop *L);
void setStripMiningOriginalTripCountMetadata(llvm::Loop *L, uint64_t tripCount);

void setStripMiningKRoleMetadata(llvm::Instruction *I, llvm::StringRef role);
std::optional<std::string> getStripMiningKRoleMetadata(const llvm::Instruction *I);

} // namespace checkpoint
