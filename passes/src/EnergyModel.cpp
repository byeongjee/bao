#include "EnergyModel.h"

#include "llvm/IR/Instructions.h"

namespace checkpoint {

int EnergyModel::CallCost = 10;

int EnergyModel::getCost(const llvm::Instruction &I) {
    return getCost(I.getOpcode());
}

int EnergyModel::getCost(unsigned Opcode) {
    using namespace llvm;

    switch (Opcode) {
    // Simple arithmetic: cost 1
    case Instruction::Add:
    case Instruction::Sub:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
        return 1;

    // Complex arithmetic: cost 5
    case Instruction::Mul:
    case Instruction::SDiv:
    case Instruction::UDiv:
    case Instruction::SRem:
    case Instruction::URem:
        return 5;

    // Floating point: cost 8
    case Instruction::FAdd:
    case Instruction::FSub:
    case Instruction::FMul:
    case Instruction::FDiv:
    case Instruction::FRem:
        return 8;

    // Memory operations
    case Instruction::Load:
        return 4;
    case Instruction::Store:
        return 5;

    // Control flow: cost 1
    case Instruction::Br:
    case Instruction::Switch:
    case Instruction::Ret:
    case Instruction::IndirectBr:
        return 1;

    // Comparison: cost 1
    case Instruction::ICmp:
    case Instruction::FCmp:
        return 1;

    // Conversions: cost 2
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
        return 2;

    // Function call: configurable
    case Instruction::Call:
    case Instruction::Invoke:
        return CallCost;

    // PHI and Select: cost 1
    case Instruction::PHI:
    case Instruction::Select:
        return 1;

    // GEP: cost 2
    case Instruction::GetElementPtr:
        return 2;

    // Alloca: cost 1 (stack allocation)
    case Instruction::Alloca:
        return 1;

    // Atomics: cost 10 (memory barriers are expensive)
    case Instruction::AtomicRMW:
    case Instruction::AtomicCmpXchg:
    case Instruction::Fence:
        return 10;

    // Default: cost 1
    default:
        return 1;
    }
}

void EnergyModel::setCallCost(int cost) {
    CallCost = cost;
}

int EnergyModel::getCallCost() {
    return CallCost;
}

} // namespace checkpoint
