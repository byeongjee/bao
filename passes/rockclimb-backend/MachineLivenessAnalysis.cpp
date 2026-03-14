#include "MachineLivenessAnalysis.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineInstr.h"

using namespace llvm;

namespace checkpoint {

RegScanResult scanBlockForReg(const MachineBasicBlock *MBB, MCPhysReg reg,
                              const TargetRegisterInfo *TRI) {
    for (const MachineInstr &MI : *MBB) {
        bool hasUse = false;
        bool hasDef = false;
        for (const MachineOperand &MO : MI.operands()) {
            if (!MO.isReg() || !MO.getReg().isPhysical())
                continue;
            if (!TRI->regsOverlap(MO.getReg(), reg))
                continue;
            if (MO.isUse() && !MO.isUndef())
                hasUse = true;
            if (MO.isDef())
                hasDef = true;
        }
        // Tied operand (use+def) counts as use-first: the instruction
        // reads the old value before writing the new one.
        if (hasUse)
            return RegScanResult::UsedFirst;
        if (hasDef)
            return RegScanResult::DefinedFirst;
    }
    return RegScanResult::Neither;
}

bool isRegLiveFromBlock(const MachineBasicBlock *startMBB, MCPhysReg reg,
                        const TargetRegisterInfo *TRI) {
    SmallVector<const MachineBasicBlock *, 8> worklist;
    SmallPtrSet<const MachineBasicBlock *, 16> visited;
    worklist.push_back(startMBB);

    while (!worklist.empty()) {
        const MachineBasicBlock *MBB = worklist.pop_back_val();
        if (!visited.insert(MBB).second)
            continue;

        RegScanResult result = scanBlockForReg(MBB, reg, TRI);
        if (result == RegScanResult::UsedFirst)
            return true;
        if (result == RegScanResult::DefinedFirst)
            continue; // killed on this path, don't follow successors

        // Neither: register passes through — follow successors
        for (const MachineBasicBlock *succ : MBB->successors())
            worklist.push_back(succ);
    }
    return false;
}

} // namespace checkpoint
