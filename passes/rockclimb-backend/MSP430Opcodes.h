#pragma once

/// Locally-defined MSP430 opcode and register constants for out-of-tree use.
/// These values are extracted from MSP430GenInstrInfo.inc and
/// MSP430GenRegisterInfoEnums.inc for LLVM 23.x.
///
/// Verified at runtime in RockClimbMachineInstrumenter::verifyConstants().

namespace msp430 {

// Register enum values (from MSP430GenRegisterInfoEnums.inc)
// Physical register IDs used by the MSP430 target.
enum Register : unsigned {
    CG = 1, // Constant generator (reserved)
    PC = 3, // Program counter (reserved)
    SP = 5, // Stack pointer (reserved)
    SR = 7, // Status register (reserved)
    R4 = 9, // Frame pointer / callee-saved
    R5 = 10,
    R6 = 11,
    R7 = 12,
    R8 = 13,
    R9 = 14,
    R10 = 15,
    R11 = 16, // Caller-saved / argument
    R12 = 17, // First argument / return value
    R13 = 18, // Second argument
    R14 = 19, // Third argument
    R15 = 20, // Fourth argument
};

// Instruction opcode enum values (from MSP430GenInstrInfo.inc)
enum Opcode : unsigned {
    CALLi = 473,   // Call with immediate/symbol operand
    MOV16ri = 534, // Move immediate to 16-bit register
    MOV16rr = 538, // Move 16-bit register to register
};

} // namespace msp430
