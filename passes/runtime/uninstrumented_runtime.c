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

#ifdef DEVICE_DEBUG

#include "debug_common.h"

/* NVM symbols for benchmark result readback.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;

void debug_exit(int result) {
    debug_exit_begin(result);
    debug_exit_end();
}

#endif /* DEVICE_DEBUG */
