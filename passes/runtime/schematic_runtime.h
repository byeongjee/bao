/*
 * SCHEMATIC Runtime Library for MSP430
 *
 * All-FRAM operation with save-and-halt at region boundaries.
 * Unlike RockClimb (which saves registers distributed at machine level),
 * SCHEMATIC bulk-saves all R4-R15 in __region_boundary since it operates
 * pre-regalloc and cannot do per-instruction register checkpointing.
 *
 * Recovery from power failures handled by schematic_boot.S.
 */

#ifndef SCHEMATIC_RUNTIME_H
#define SCHEMATIC_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Maximum number of registers to checkpoint (MSP430 has R4-R15 = 12 GPRs) */
#define SCHEMATIC_MAX_REGS 16

/* ============================================================================
 * NVM Storage (placed in non-volatile memory section)
 * ============================================================================ */

/* Section attribute for NVM placement (ELF targets only) */
#if defined(__ELF__)
#define NVM_SECTION __attribute__((section(".nvm")))
#else
#define NVM_SECTION
#endif

/* Checkpointed register values */
extern uint16_t __nvm_regs[SCHEMATIC_MAX_REGS] NVM_SECTION;

/* Saved program counter for recovery */
extern uint16_t __nvm_pc NVM_SECTION;

/* Saved stack pointer for recovery */
extern uint16_t __nvm_sp NVM_SECTION;

#ifdef DEBUG_COUNTERS
/* NVM Debug Counters (incremented in assembly) */
extern uint16_t cnt_boundary NVM_SECTION;
extern uint16_t cnt_save_reg NVM_SECTION;
extern uint16_t cnt_restore_reg NVM_SECTION;
extern uint16_t cnt_store_mem NVM_SECTION;
extern uint16_t cnt_restore_mem NVM_SECTION;
#endif

/* ============================================================================
 * Runtime API
 * ============================================================================ */

/**
 * Region boundary: bulk-save registers, save state, and halt (deep sleep).
 *
 * Saves R4-R15 to NVM, then saves return address (region body start) and SP,
 * then enters LPM4. System powers off. On reboot, boot.S recovers from
 * saved state.
 *
 * Provided by schematic_boot.S (assembly).
 */
void __region_boundary(void);

#ifdef __cplusplus
}
#endif

#endif /* SCHEMATIC_RUNTIME_H */
