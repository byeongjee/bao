#pragma once

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace checkpoint {

/// MIR-level pass that assigns unique DWARF line numbers to each MachineBasicBlock.
///
/// This is the machine-level counterpart of the IR-level assign-bb-debuginfo pass.
/// It overwrites each MachineInstr's DebugLoc so that the DWARF line number encodes
/// the MIR BB number (1-based). A mapping JSON is emitted so that bb-energy-analyzer
/// can map DWARF lines back to MIR BB names.
///
/// Run after virtregrewriter, before rockclimb:
///   llc -run-pass=assign-mir-bb-debuginfo -mir-bb-mapping=mapping.json
class AssignMIRBBDebugInfoPass : public llvm::MachineFunctionPass {
  public:
    static char ID;

    AssignMIRBBDebugInfoPass();

    bool runOnMachineFunction(llvm::MachineFunction &MF) override;

    llvm::StringRef getPassName() const override { return "Assign MIR BB Debug Info"; }
};

} // namespace checkpoint
