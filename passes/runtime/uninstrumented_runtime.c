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

/* Completion flag (unconditional — set by BENCH_EXIT via
   bench_commit_result). No checkpointing here, so it is informational only.
   Explicit zero initializer forces PROGBITS so the flash programmer
   writes zero on first flash. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;

/* The run's return value, read by the host via mspdebug md. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;

/* Called by BENCH_EXIT before the end pulse (see benchmark.h). */
void bench_commit_result(int result) {
    __nvm_result = (uint16_t)result;
    __nvm_done = 1;
}

#ifdef DEVICE_DEBUG

#include "debug_common.h"

void debug_exit(int result) {
    debug_exit_begin(result);
    debug_exit_end();
}

#endif /* DEVICE_DEBUG */
