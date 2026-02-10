/*
 * RockClimb Runtime Library for MSP430
 *
 * This runtime implements the PFI (Proactive Failure Immunity) approach
 * from the RockClimb paper. Key features:
 * - Voltage monitoring at region boundaries
 * - Low-power mode entry when voltage is too low
 * - Register checkpointing to NVM
 * - Recovery from power failures
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

/* Default voltage threshold (ADC value, platform-specific) */
#define ROCKCLIMB_DEFAULT_VMAX_THRESHOLD 0x1FF

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

/* Voltage threshold for V_max comparison */
extern volatile uint16_t __rockclimb_vmax_threshold;

/* ============================================================================
 * Runtime API
 * ============================================================================ */

/**
 * Voltage check function - called at region boundaries.
 *
 * Algorithm (PFI approach):
 * 1. Check if current voltage is above V_max threshold
 * 2. If below threshold, enter low-power mode (LPM4)
 * 3. Wait for voltage comparator interrupt when voltage rises
 * 4. Increment region ID and continue execution
 *
 * This is a rollback-free approach: we wait for sufficient energy
 * rather than re-executing after power failure.
 */
void __rockclimb_check(void);

/**
 * Save a single register to NVM.
 *
 * Called at the last definition point of each live-out register
 * within a region (distributed checkpointing strategy).
 *
 * @param reg_id  Register identifier (0 to ROCKCLIMB_MAX_REGS-1)
 * @param value   Register value to save
 */
void __rockclimb_save_reg(uint8_t reg_id, uint16_t value);

/**
 * Initialize the RockClimb runtime.
 *
 * Should be called early in main() or from crt0.
 * Sets up voltage comparator and clears NVM if this is a fresh boot.
 */
void __rockclimb_init(void);

/**
 * Check if this is a recovery boot.
 *
 * @return Non-zero if recovering from power failure, 0 otherwise.
 */
uint16_t __rockclimb_is_recovery(void);

/**
 * Perform recovery after power failure.
 *
 * Restores registers from NVM and returns to saved PC.
 * This function does not return normally.
 */
void __rockclimb_recover(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* ROCKCLIMB_RUNTIME_H */
