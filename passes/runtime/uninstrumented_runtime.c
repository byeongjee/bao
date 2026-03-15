/*
 * Uninstrumented Runtime for MSP430FR5994
 *
 * Minimal runtime for the uninstrumented baseline.
 * No checkpoint functions, no UART, no counters.
 *
 * When compiled with -DDEVICE_DEBUG, provides:
 *   - NVM result/done symbols for benchmark result readback
 *   - debug_init() / debug_exit() API
 */

#include <stdint.h>

#ifdef DEVICE_DEBUG

#include "benchmark.h"
#include <msp430.h>

/* NVM symbols for benchmark result readback.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;

void debug_init(void) {
    WDTCTL = WDTPW | WDTHOLD;

    /* If already completed, halt permanently (prevents re-execution
     * when mspdebug reconnects and resets the device). */
    while (__nvm_done == 1) {
        __bis_SR_register(LPM4_bits);
    }
}

void debug_exit(int result) {
    __nvm_result = (uint16_t)result;
    __nvm_done = 1;
    __bis_SR_register(LPM4_bits);
}

#endif /* DEVICE_DEBUG */
