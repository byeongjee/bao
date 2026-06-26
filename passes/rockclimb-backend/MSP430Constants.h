#pragma once

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/MC/MCRegister.h"

namespace checkpoint {

/// MSP430 opcode / register / register-class constants resolved by NAME at
/// runtime from TargetInstrInfo / TargetRegisterInfo.
///
/// The MSP430 target's instruction and register enums are not part of LLVM's
/// public, installed API, so out-of-tree code cannot reference them directly
/// and must not hardcode their numeric values: those values shift whenever the
/// instruction table is regenerated (e.g. between LLVM versions), which
/// silently points an opcode at the wrong instruction and corrupts codegen.
///
/// Names are stable across LLVM revisions, so we resolve the handful we need by
/// name. Any unresolved name aborts loudly (report_fatal_error) rather than
/// proceeding with a wrong value.
struct MSP430Constants {
    // Opcodes
    unsigned MOV16mr = 0;  // mov.w  reg -> mem
    unsigned ADD16mi = 0;  // add.w  #imm, mem
    unsigned ADDC16mc = 0; // addc.w #const, mem
    unsigned PUSH16r = 0;
    unsigned POP16r = 0;
    unsigned CALLi = 0;

    // Registers
    llvm::MCRegister SR;  // status register
    llvm::MCRegister R4;  // lowest checkpointable GPR
    llvm::MCRegister R15; // highest checkpointable GPR

    // Sub-register index and register-class IDs
    unsigned subreg_8bit = 0;
    unsigned GR8RegClassID = 0;
    unsigned GR16RegClassID = 0;

    /// Resolve all constants for the MSP430 subtarget of *MF*. Aborts if any
    /// name is missing or if R4..R15 are not 12 contiguous registers (the
    /// __nvm_regs slot mapping `id = reg - R4` and boot.S depend on that).
    static MSP430Constants resolve(const llvm::MachineFunction &MF);
};

} // namespace checkpoint
