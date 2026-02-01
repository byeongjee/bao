/*
 * MILP Runtime Stubs for Testing
 *
 * Minimal stubs for running checkpointed code under constant power.
 * These do nothing but allow the instrumented program to link and run.
 */

#include <stdint.h>

#include "milp_runtime.h"
#include "../include/rockclimb_debug.h"

void __milp_checkpoint(const char *block_name) {
    static uint32_t call_count = 0;
    static uint8_t debug_inited = 0;

    call_count++;

    if (!debug_inited) {
        debug_init();
        debug_inited = 1;
    }

    DEBUG_PRINT("[MILP] __milp_checkpoint call #%lu at %s\n",
                (unsigned long)call_count,
                (block_name ? block_name : "(null)"));
}
