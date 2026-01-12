#include "EnergyModel.h"
#include "EnergyConfig.h"

#include "llvm/IR/Instructions.h"

namespace checkpoint {

int EnergyModel::getCost(const llvm::Instruction &I) {
    return getCost(I.getOpcode());
}

int EnergyModel::getCost(unsigned Opcode) {
    using namespace llvm;

    switch (Opcode) {
    // Simple arithmetic
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
        return EnergyConfig::getInstructionCost("simple_arithmetic");

    // Complex arithmetic
    case Instruction::Mul:
    case Instruction::SDiv:
    case Instruction::UDiv:
    case Instruction::SRem:
    case Instruction::URem:
        return EnergyConfig::getInstructionCost("complex_arithmetic");

    // Floating point
    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
        return EnergyConfig::getInstructionCost("floating_point");

    // Memory operations
    case Instruction::Load:
        return EnergyConfig::getInstructionCost("load");
    case Instruction::Store:
        return EnergyConfig::getInstructionCost("store");

    // Control flow
    case Instruction::Br:
    case Instruction::Switch:
    case Instruction::Ret:
    case Instruction::IndirectBr:
        return EnergyConfig::getInstructionCost("control_flow");

    // Comparison
    case Instruction::ICmp:
    case Instruction::FCmp:
        return EnergyConfig::getInstructionCost("comparison");

    // Conversions
    case Instruction::Trunc:
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::UIToFP:
    case Instruction::SIToFP:
    case Instruction::FPTrunc:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::BitCast:
    case Instruction::AddrSpaceCast:
        return EnergyConfig::getInstructionCost("conversion");

    // Function call
    case Instruction::Call:
    case Instruction::Invoke:
        return EnergyConfig::getInstructionCost("call");

    // PHI and Select
    case Instruction::PHI:
    case Instruction::Select:
        return EnergyConfig::getInstructionCost("phi_select");

    // GEP
    case Instruction::GetElementPtr:
        return EnergyConfig::getInstructionCost("gep");

    // Alloca
    case Instruction::Alloca:
        return EnergyConfig::getInstructionCost("alloca");

    // Atomics
    case Instruction::AtomicRMW:
    case Instruction::AtomicCmpXchg:
    case Instruction::Fence:
        return EnergyConfig::getInstructionCost("atomic");

    // Default
    default:
        return EnergyConfig::getInstructionCost("default");
    }
}

} // namespace checkpoint
