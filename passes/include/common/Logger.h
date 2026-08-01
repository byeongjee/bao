#pragma once

#define PLOG_LOCAL    // keep plog symbols private to each shared library
#include <plog/Log.h> // IWYU pragma: export

// Bridge LLVM types to plog's << operator (plog uses std::ostream, not raw_ostream)
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

namespace plog {
inline Record &operator<<(Record &r, llvm::StringRef sr) {
    return r << std::string_view(sr.data(), sr.size());
}
inline Record &operator<<(Record &r, const llvm::Twine &t) {
    return r << t.str();
}
} // namespace plog

namespace checkpoint {

/// Format a double to a fixed-precision string (replacement for llvm::format
/// which only works with raw_ostream, not std::ostream used by plog).
inline std::string fmtDouble(double val, int precision = 3) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, val);
    return buf;
}

/// Parse -ckpt-log-level cl::opt and initialize plog.
/// Safe to call multiple times (idempotent).
void initLogging();

} // namespace checkpoint
