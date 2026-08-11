/*
 * Shared debug infrastructure for MSP430 checkpoint runtimes.
 *
 * Compiled with -DDEVICE_DEBUG by the build system. Provides UART
 * output and the debug_init/debug_exit skeleton shared by all
 * instrumented runtimes (MILP, RockClimb, SCHEMATIC).
 */

#ifdef DEVICE_DEBUG

#include "debug_common.h"
#include "benchmark.h"
#include <msp430.h>

/* These symbols are defined in each variant's *_runtime.c */
extern volatile uint16_t __nvm_result;
extern volatile uint16_t __nvm_done;
extern volatile uint16_t __nvm_in_region;

/* ============================================================================
 * UART Setup for MSP430FR5994
 *
 * Uses eUSCI_A0 at 9600 baud (assuming SMCLK from DCO at F_CPU).
 * ============================================================================ */

void uart_init(void) {
    PM5CTL0 &= ~LOCKLPM5;

    /* Configure UART pins: P2.0 = UCA0TXD, P2.1 = UCA0RXD */
    P2SEL1 |= BIT0 | BIT1;
    P2SEL0 &= ~(BIT0 | BIT1);

    /* Configure eUSCI_A0 for 9600 baud.
     * DCO is configured by timing_gpio_init() or boot recovery.
     * Baud rate values from TI UG Table 30-5 (oversampling). */
    UCA0CTLW0 = UCSWRST; /* Hold in reset */
    UCA0CTLW0 |= UCSSEL__SMCLK;
#if F_CPU == 16000000UL
    UCA0BRW = 104;
    UCA0MCTLW = UCOS16 | UCBRF_2 | 0xD600;
#elif F_CPU == 8000000UL
    UCA0BRW = 52;
    UCA0MCTLW = UCOS16 | UCBRF_1 | 0x4900;
#elif F_CPU == 1000000UL
    UCA0BRW = 6;
    UCA0MCTLW = UCOS16 | UCBRF_8 | 0x2000;
#else
#error "Unsupported F_CPU for UART baud rate"
#endif
    UCA0CTLW0 &= ~UCSWRST; /* Release reset */
}

/* Direct UART character/string output (no newlib stdio dependency).
 * Timeout prevents infinite blocking when JTAG holds the debug interface
 * or when no serial connection is present. */
void uart_putc(char c) {
    volatile uint16_t timeout = 10000;
    while (!(UCA0IFG & UCTXIFG) && --timeout)
        ;
    if (timeout)
        UCA0TXBUF = c;
}

void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

void uart_put_u16(uint16_t val) {
    char buf[6]; /* max 5 digits + null */
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    if (val == 0) {
        *--p = '0';
    } else {
        while (val > 0) {
            *--p = '0' + (char)(val % 10);
            val /= 10;
        }
    }
    uart_puts(p);
}

void uart_put_u32(uint32_t val) {
    char buf[11]; /* max 10 digits + null */
    char *p = buf + sizeof(buf) - 1;
    *p = '\0';
    if (val == 0) {
        *--p = '0';
    } else {
        while (val > 0) {
            *--p = '0' + (char)(val % 10);
            val /= 10;
        }
    }
    uart_puts(p);
}

/* ============================================================================
 * Debug API
 * ============================================================================ */

void debug_init(void) {
    WDTCTL = WDTPW | WDTHOLD; /* Stop watchdog — must be first */

    /* If program already completed, halt permanently in a loop.
     * Prevents NVM values from being overwritten when mspdebug reconnects
     * (tilib driver resets the device on connect, re-running main).
     * Volatile loop: survives JTAG wake-ups (re-enters LPM4 immediately).
     * __nvm_done is volatile, so the compiler cannot optimize this away. */
    while (__nvm_done == 1) {
        __bis_SR_register(LPM4_bits);
    }
    /* Configure DCO first so SMCLK is at F_CPU before UART baud rate setup.
     * timing_gpio_init() sets DCO to F_CPU and configures the GPIO pin. */
    timing_gpio_init();
    uart_init();
    uart_puts("BOOT\r\n");
}

void debug_exit_commit(int result) {
    __nvm_in_region = 0;
    __nvm_result = (uint16_t)result;
    __nvm_done = 1;
}

void debug_exit_begin(int result) {
    /* UART output kept for manual debugging */
    uart_init(); /* Re-init UART — may be uninitialized after BOR/LPM4 recovery */
    uart_puts("RESULT: ");
    uart_put_u16((uint16_t)result);
    uart_puts("\r\n");
    uart_puts("=== Debug Counter Summary ===\r\n");
}

void debug_exit_end(void) {
    uart_puts("\n" DEBUG_END_MARKER "\r\n");

    /* Halt CPU — unblocks mspdebug "run" command for NVM readback */
    __bis_SR_register(LPM4_bits);
}

#endif /* DEVICE_DEBUG */
