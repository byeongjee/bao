/*
 * RockClimb Runtime Stubs for Testing
 *
 * Minimal stubs for testing checkpointed code under constant power.
 * These do nothing but allow the code to run for verification.
 */

#include <stdint.h>

/* NVM region ID - used by recovery dispatcher */
uint32_t __nvm_region_id = 0;

/* Voltage check stub - does nothing under constant power */
void __rockclimb_check(void) {
    /* Under constant power, just return immediately */
}
