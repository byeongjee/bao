/*
 * RockClimb Runtime Stubs for Testing
 *
 * Minimal stubs for testing checkpointed code under constant power.
 * These do nothing but allow the code to run for verification.
 */

#include <stdint.h>
#include "../include/rockclimb_debug.h"

/* NVM region ID - used by recovery dispatcher */
uint32_t __nvm_region_id = 0;

/* Voltage check stub - does nothing under constant power */
void __rockclimb_check(void) {
    static uint32_t check_count = 0;
    check_count++;
    DEBUG_PRINT("[RC] __rockclimb_check call #%u\n", check_count);
}

/* Register save stub - does nothing under constant power */
void __rockclimb_save_reg(int reg_id, int value) {
    /* Under constant power, skip register checkpointing */
    (void)reg_id;
    (void)value;
}
