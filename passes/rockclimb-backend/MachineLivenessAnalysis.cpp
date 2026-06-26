#include "MachineLivenessAnalysis.h"

#include "MSP430Constants.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

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

DenseMap<const MachineBasicBlock *, SmallSet<MCPhysReg, 12>>
computeBulkLiveIn(const MachineFunction &MF, const TargetRegisterInfo *TRI) {
    const MachineRegisterInfo &MRI = MF.getRegInfo();
    const MSP430Constants C = MSP430Constants::resolve(MF);

    // Per-block use-before-def and def sets (checkpointable regs only)
    DenseMap<const MachineBasicBlock *, SmallSet<MCPhysReg, 12>> useSet, defSet;
    for (const MachineBasicBlock &MBB : MF) {
        auto &uses = useSet[&MBB];
        auto &defs = defSet[&MBB];
        for (const MachineInstr &MI : MBB) {
            for (const MachineOperand &MO : MI.operands()) {
                if (!MO.isReg() || !MO.getReg().isPhysical())
                    continue;
                MCPhysReg reg = MO.getReg().asMCReg();
                if (MRI.isReserved(reg))
                    continue;
                // Normalize sub-regs to their 16-bit GPR
                MCPhysReg gpr = 0;
                for (unsigned r = C.R4.id(); r <= C.R15.id(); ++r) {
                    if (TRI->regsOverlap(reg, r)) {
                        gpr = static_cast<MCPhysReg>(r);
                        break;
                    }
                }
                if (!gpr)
                    continue;
                if (MO.isUse() && !MO.isUndef() && !defs.count(gpr))
                    uses.insert(gpr);
                if (MO.isDef())
                    defs.insert(gpr);
            }
        }
    }

    // Initialize liveIn from use sets
    DenseMap<const MachineBasicBlock *, SmallSet<MCPhysReg, 12>> liveIn;
    for (const MachineBasicBlock &MBB : MF)
        liveIn[&MBB] = useSet[&MBB];

    // Iterate until fixed point
    bool changed = true;
    while (changed) {
        changed = false;
        for (const MachineBasicBlock &MBB : MF) {
            // liveOut = union of successor liveIns
            SmallSet<MCPhysReg, 12> liveOut;
            for (const MachineBasicBlock *succ : MBB.successors())
                for (MCPhysReg reg : liveIn[succ])
                    liveOut.insert(reg);

            // liveIn = use ∪ (liveOut \ def)
            auto &li = liveIn[&MBB];
            for (MCPhysReg reg : liveOut) {
                if (!defSet[&MBB].count(reg)) {
                    if (li.insert(reg).second)
                        changed = true;
                }
            }
        }
    }

    return liveIn;
}

} // namespace checkpoint
