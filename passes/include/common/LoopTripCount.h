#pragma once

#include "llvm/Analysis/LoopInfo.h"

#include <optional>

namespace checkpoint {

/// Return the trip count from !llvm.loop metadata (llvm.loop.tripcount.upper),
/// or std::nullopt if no such metadata is present.
std::optional<uint64_t> getMarkerTripCount(const llvm::Loop *L);

/// Attach (or replace) a llvm.loop.tripcount.upper entry in the loop's
/// !llvm.loop metadata.  Preserves any other existing metadata entries.
void setLoopTripCountMetadata(llvm::Loop *L, uint64_t tripCount);

/// Remove the llvm.loop.tripcount.upper entry from the loop's !llvm.loop
/// metadata.  If that was the only entry, the loop ID is cleared entirely.
void removeLoopTripCountMetadata(llvm::Loop *L);

/// Return true if the loop is marked as produced by strip-mining.
bool hasStripMinedLoopMetadata(const llvm::Loop *L);

/// Mark loop metadata to indicate this loop is the strip-mined K-bounded loop.
void setStripMinedLoopMetadata(llvm::Loop *L);

/// Remove strip-mined marker metadata from the loop.
void removeStripMinedLoopMetadata(llvm::Loop *L);

} // namespace checkpoint
