/*
 * Uninstrumented Runtime for MSP430FR5994
 *
 * Minimal runtime for the uninstrumented baseline. Provides only the
 * NVM result/done symbols needed for benchmark result readback.
 * No checkpoint functions.
 */

#include <stdint.h>

/* NVM symbols for benchmark result readback.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash. */
__attribute__((section(".nvm"))) uint16_t __nvm_done = 0;
__attribute__((section(".nvm"))) uint16_t __nvm_result = 0;
