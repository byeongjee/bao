/*
 * Uninstrumented Debug Counters Runtime (MSP430)
 *
 * Minimal debug_init/debug_exit for uninstrumented baselines.
 * NVM symbols (__nvm_done, __nvm_result) are defined in
 * uninstrumented_runtime.c.
 *
 * Benchmarks call debug_init() at start of main() and debug_exit()
 * at end. Guarded by DEBUG_COUNTERS macro — compiles to no-ops without it.
 */

#include <msp430.h>
#include <stdint.h>

#include "benchmark.h"

extern volatile uint16_t __nvm_result;
extern volatile uint16_t __nvm_done;

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
