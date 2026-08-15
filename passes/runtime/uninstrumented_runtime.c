/*
 * Uninstrumented Runtime for MSP430FR5994
 *
 * Minimal runtime for the uninstrumented baseline.
 * No checkpoint functions, no counters.
 *
 * When compiled with -DDEVICE_DEBUG, provides:
 *   - NVM result/done symbols for benchmark result readback
 *   - debug_exit() (no counters to print)
 *   - debug_init() and UART from debug_common.c
 */

#include <msp430.h>
#include <stdint.h>

void bench_halt(void) {
    __bis_SR_register(LPM4_bits);
}

/* Completion flag (unconditional — set by non-debug BENCH_EXIT via
   bench_commit_done). No checkpointing here, so it is informational only.
   Explicit zero initializer forces PROGBITS so the flash programmer
   writes zero on first flash. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;

/* No regions in the baseline; defined because the shared exit code
   (debug_exit_commit in debug_common.c) clears it. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_in_region = 0;

/* Called by non-debug BENCH_EXIT before the end pulse (see benchmark.h). */
void bench_commit_done(void) {
    __nvm_done = 1;
}

#ifdef DEVICE_DEBUG

#include "debug_common.h"

/* NVM result storage for benchmark result readback. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;

void debug_exit(int result) {
    debug_exit_begin(result);
    debug_exit_end();
}

#endif /* DEVICE_DEBUG */
