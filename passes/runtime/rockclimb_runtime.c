/*
 * RockClimb Runtime Library Implementation for MSP430FR5994
 *
 * All-FRAM operation: mutable data lives in FRAM, stack in FRAM.
 * Region boundaries save state and halt (deep sleep). On reboot,
 * boot.S recovers from NVM and resumes execution.
 *
 * __rockclimb_check is provided by rockclimb_boot.S (assembly).
 */

#include "rockclimb_runtime.h"
#include <msp430.h>

/* ============================================================================
 * NVM Storage Definitions
 * ============================================================================ */

/* Checkpointed register values */
__attribute__((section(".nvm"))) uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS];

/* Current region ID */
__attribute__((section(".nvm"))) uint16_t __nvm_region_id;

/* Saved program counter */
__attribute__((section(".nvm"))) uint16_t __nvm_pc;

/* Saved stack pointer */
__attribute__((section(".nvm"))) uint16_t __nvm_sp;

/* ============================================================================
 * Runtime Implementation
 * ============================================================================ */

/* __rockclimb_check is provided by rockclimb_boot.S */

void __rockclimb_save_reg(void) {}

void __region_prologue(void) {}

void __region_epilogue(void) {}

void __checkpoint_store_reg(int32_t slot_id, int64_t value) {
    (void)slot_id;
    (void)value;
}

/* No-op stub for loop trip-count annotations in instrumented IR. */
void __loop_tripcount(int max_iterations) {
    (void)max_iterations;
}

void __rockclimb_init(void) {
    /* Stop watchdog timer */
    WDTCTL = WDTPW | WDTHOLD;

    /* Check if this is a fresh boot - FRAM erased state is 0xFFFF */
    if (__nvm_region_id == 0xFFFF) {
        __nvm_region_id = 0;
        __nvm_pc = 0;
        __nvm_sp = 0;

        for (uint8_t i = 0; i < ROCKCLIMB_MAX_REGS; i++) {
            __nvm_regs[i] = 0;
        }
    }
}

uint16_t __rockclimb_is_recovery(void) {
    return (__nvm_region_id > 0 && __nvm_pc != 0);
}

void __rockclimb_recover(void) {
    /*
     * Recovery is handled by boot.S (.crt_0010) before this code runs.
     * This function exists only to satisfy the API; it should never be called.
     */
    while (1)
        ;
}
