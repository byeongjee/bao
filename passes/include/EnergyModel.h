#pragma once

#include "llvm/IR/Instruction.h"

namespace checkpoint {

/// Energy cost model for LLVM IR instructions.
/// Assigns energy costs to opcodes based on typical embedded processor costs.
class EnergyModel {
public:
    /// Get energy cost for an instruction.
    static int getCost(const llvm::Instruction &I);

    /// Get energy cost for an opcode.
    static int getCost(unsigned Opcode);

    /// Set the cost for call instructions (default: 10).
    static void setCallCost(int cost);

    /// Get the current call cost.
    static int getCallCost();

private:
    static int CallCost;
};

} // namespace checkpoint
