#pragma once

#include "llvm/ADT/StringRef.h"

namespace checkpoint {

inline bool isBenchmarkInfrastructureFunction(llvm::StringRef name) {
    return name.starts_with("timing_gpio") || name.starts_with("_timing_delay") ||
           name.starts_with("debug_") || name.starts_with("uart_");
}

} // namespace checkpoint
