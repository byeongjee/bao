/*
 * Shared debug infrastructure for MSP430 checkpoint runtimes.
 *
 * Provides UART output and debug_init/debug_exit helpers.
 * Used by milp_runtime.c, rockclimb_runtime.c, schematic_runtime.c
 * when compiled with -DDEVICE_DEBUG.
 */

#ifndef DEBUG_COMMON_H
#define DEBUG_COMMON_H

#include <stdint.h>

#ifdef DEVICE_DEBUG

/*
 * UART output functions for MSP430FR5994.
 * Uses eUSCI_A0 at 9600 baud. F_CPU must be defined.
 */
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_put_u16(uint16_t val);

/*
 * debug_init — Call at benchmark start (via BENCH_INIT).
 *
 * Stops WDT, halts if program already completed (prevents re-execution
 * on mspdebug reconnect), inits UART, prints "BOOT".
 *
 * Requires __nvm_done to be defined by the variant's runtime.c.
 */
void debug_init(void);

/*
 * debug_exit_begin — Start the exit sequence.
 *
 * Stores result in __nvm_result/__nvm_done, re-inits UART,
 * prints "RESULT: <val>" and the counter summary header.
 *
 * After calling this, the variant prints its specific counters,
 * then calls debug_exit_end().
 *
 * Requires __nvm_result and __nvm_done to be defined by the
 * variant's runtime.c.
 */
void debug_exit_begin(int result);

/*
 * debug_exit_end — Finish the exit sequence.
 *
 * Prints DEBUG_END_MARKER and halts the CPU in LPM4.
 */
void debug_exit_end(void);

#endif /* DEVICE_DEBUG */

#endif /* DEBUG_COMMON_H */
