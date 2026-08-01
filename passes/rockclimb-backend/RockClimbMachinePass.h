#pragma once

#include "llvm/CodeGen/MachineFunctionPass.h"

namespace checkpoint {

/// Runtime/instrumentation functions that the pass never checkpoints
/// (and that therefore never write __nvm_regs).
bool isRuntimeFunction(llvm::StringRef name);

class RockClimbMachinePass : public llvm::MachineFunctionPass {
  public:
    static char ID;

    RockClimbMachinePass();

    bool runOnMachineFunction(llvm::MachineFunction &MF) override;

    void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;

    llvm::StringRef getPassName() const override {
        return "RockClimb Machine-Level Checkpoint Insertion";
    }
};

} // namespace checkpoint
