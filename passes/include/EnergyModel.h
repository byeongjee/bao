#pragma once

#include "llvm/IR/Instruction.h"

namespace checkpoint {

/// Energy cost model for LLVM IR instructions.
/// Uses costs from EnergyConfig (loaded from JSON).
class EnergyModel {
public:
    /// Get energy cost for an instruction.
    static int getCost(const llvm::Instruction &I);

    /// Get energy cost for an opcode.
    static int getCost(unsigned Opcode);
};

} // namespace checkpoint
