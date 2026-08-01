#include "MSP430Constants.h"

#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace checkpoint {

static unsigned resolveOpcode(const TargetInstrInfo &TII, StringRef name) {
    for (unsigned op = 0, e = TII.getNumOpcodes(); op < e; ++op)
        if (TII.getName(op) == name)
            return op;
    report_fatal_error(Twine("MSP430Constants: opcode '") + name +
                       "' not found; "
                       "the loaded LLVM does not match this plugin's expectations");
}

static MCRegister resolveRegister(const TargetRegisterInfo &TRI, StringRef name) {
    // TRI->getName returns the TableGen def name (e.g. "R4", "SR"); match
    // case-insensitively to be robust to asm-name vs def-name differences.
    for (unsigned reg = 1, e = TRI.getNumRegs(); reg < e; ++reg)
        if (name.equals_insensitive(TRI.getName(reg)))
            return MCRegister(reg);
    report_fatal_error(Twine("MSP430Constants: register '") + name + "' not found");
}

MSP430Constants MSP430Constants::resolve(const MachineFunction &MF) {
    const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
    const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();

    MSP430Constants c;
    c.MOV16mr = resolveOpcode(TII, "MOV16mr");
    c.CALLi = resolveOpcode(TII, "CALLi");

    c.SR = resolveRegister(TRI, "SR");
    c.R4 = resolveRegister(TRI, "R4");
    c.R11 = resolveRegister(TRI, "R11");
    c.R15 = resolveRegister(TRI, "R15");

    // The __nvm_regs slot mapping (id = reg - R4) and boot.S's restore order
    // assume R4..R15 are 12 registers numbered contiguously and in order.
    if (c.R15.id() < c.R4.id() || (c.R15.id() - c.R4.id()) != 11)
        report_fatal_error("MSP430Constants: R4..R15 are not 12 contiguous registers");

    return c;
}

} // namespace checkpoint
