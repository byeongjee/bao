/*
 * RockClimb Runtime Library for MSP430
 *
 * All-FRAM operation with save-and-halt at region boundaries.
 * Register checkpointing to NVM, recovery from power failures
 * handled by boot.S.
 */

#ifndef ROCKCLIMB_RUNTIME_H
#define ROCKCLIMB_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/* Maximum number of registers to checkpoint (MSP430 has R4-R15 = 12 GPRs) */
#define ROCKCLIMB_MAX_REGS 16

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
extern uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS] NVM_SECTION;

/* Saved program counter for recovery */
extern uint16_t __nvm_pc NVM_SECTION;

/* Saved stack pointer for recovery */
extern uint16_t __nvm_sp NVM_SECTION;

#ifdef DEVICE_DEBUG
/* NVM Debug Counters */
extern uint32_t cnt_boundary NVM_SECTION;
extern uint32_t cnt_restore_reg NVM_SECTION;
#endif

/* ============================================================================
 * Runtime API
 * ============================================================================ */

/**
 * Region boundary: save state and halt.
 *
 * Saves return address (region body start) and SP to NVM. Halt behavior
 * depends on the compile-time mode:
 *   - swbor (HALT_SWBOR, default): jumps to _start without destroying state
 *   - bor   (HALT_BOR): triggers reset and destroys modeled volatile state
 *   - lpm4  (HALT_LPM4): enters LPM4 deep sleep (real deployment)
 *
 * On reboot, boot.S recovers from saved NVM state.
 *
 * Provided by rockclimb_boot.S (assembly).
 */
void __region_boundary(void);

#ifdef __cplusplus
}
#endif

#endif /* ROCKCLIMB_RUNTIME_H */
