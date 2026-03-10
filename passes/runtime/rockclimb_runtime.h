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

/* Current region ID (incremented at each boundary) */
extern uint16_t __nvm_region_id NVM_SECTION;

/* Saved program counter for recovery */
extern uint16_t __nvm_pc NVM_SECTION;

/* Saved stack pointer for recovery */
extern uint16_t __nvm_sp NVM_SECTION;

/* ============================================================================
 * Runtime API
 * ============================================================================ */

/**
 * Region boundary: save state and halt (deep sleep).
 *
 * Saves return address (region body start) and SP to NVM, then enters
 * LPM4. System powers off. On reboot, boot.S recovers from saved state.
 *
 * Provided by rockclimb_boot.S (assembly).
 */
void __rockclimb_check(void);

/**
 * Save a single register to NVM (distributed checkpointing).
 */
void __rockclimb_save_reg(void);

/*
 * Optional debug marker API (MILP-style counters).
 * Emitted when add_debug_markers / -add-debug-markers is enabled.
 */
void __region_prologue(void);
void __region_epilogue(void);
void __checkpoint_store_reg(int32_t slot_id, int64_t value);

/**
 * Initialize the RockClimb runtime.
 * Disables watchdog and clears NVM on fresh boot.
 */
void __rockclimb_init(void);

/**
 * Check if this is a recovery boot.
 * @return Non-zero if recovering from power failure, 0 otherwise.
 */
uint16_t __rockclimb_is_recovery(void);

/**
 * Perform recovery after power failure.
 * Note: Recovery is handled by boot.S; this is a stub.
 */
void __rockclimb_recover(void)
#if defined(__ELF__)
    __attribute__((noreturn))
#endif
    ;

#ifdef __cplusplus
}
#endif

#endif /* ROCKCLIMB_RUNTIME_H */
