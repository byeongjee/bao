#pragma once

#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/MC/MCRegister.h"

namespace checkpoint {

/// Result of scanning a single block for uses/defs of a register.
enum class RegScanResult {
    UsedFirst,    ///< Register (or alias) is read before any write
    DefinedFirst, ///< Register (or alias) is written before any read
    Neither       ///< Block neither reads nor writes the register
};

/// Scan a block's instructions forward to determine if reg (or any
/// overlapping register) is used before defined, defined before used,
/// or neither.
///
/// If startAfter is non-null, skip all instructions up to and including
/// that instruction before scanning.  Used to scan from the recovery
/// point (after the boundary CALL) in the region's start block.
///
/// Handles all operand types: explicit, implicit, tied (use+def counts
/// as UsedFirst).  Skips isUndef() operands.
RegScanResult scanBlockForReg(const llvm::MachineBasicBlock *MBB, llvm::MCPhysReg reg,
                              const llvm::TargetRegisterInfo *TRI,
                              const llvm::MachineInstr *startAfter = nullptr);

/// Determine if reg is live starting from startMBB by BFS over the CFG.
/// Returns true if any reachable path uses reg before redefining it.
///
/// If startAfter is non-null, scanning of the first block begins after
/// that instruction.  This is used to check liveness from the recovery
/// point (after the boundary CALL), skipping the CALL's implicit defs
/// which don't apply during BOR recovery.
///
/// Replaces MBB->isLiveIn() which is unreliable after register
/// allocation.  Handles live-through blocks (register neither used
/// nor defined) by continuing BFS to successors.
bool isRegLiveFromBlock(const llvm::MachineBasicBlock *startMBB, llvm::MCPhysReg reg,
                        const llvm::TargetRegisterInfo *TRI,
                        const llvm::MachineInstr *startAfter = nullptr);

} // namespace checkpoint
