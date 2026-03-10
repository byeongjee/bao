/*
 * RockClimb Debug Counters Runtime (MSP430)
 *
 * Links ALONGSIDE boot.S + runtime.c (not replacing them).
 * Provides NVM debug counters for profiling and UART output.
 *
 * The RockClimb machine pass increments these counters inline using
 * ADD.W instructions (no function calls) when add_debug_markers=true.
 *
 * Counter summary is printed over UART at program exit via destructor.
 * All counters (cnt_boundary, cnt_save_reg, cnt_restore_reg) are
 * incremented inline by the instrumenter via ADD.W instructions.
 *
 * Compile with: msp430-elf-gcc -mmcu=MSP430FR5994 -msmall -O2
 */

#include <msp430.h>
#include <stdint.h>

#include "rockclimb_runtime.h"

/* ============================================================================
 * UART Setup for MSP430FR5994
 *
 * Uses eUSCI_A0 at 9600 baud (assuming 1 MHz SMCLK from DCO).
 * ============================================================================ */

static void uart_init(void) {
    /* Unlock GPIO */
    PM5CTL0 &= ~LOCKLPM5;

    /* Configure UART pins: P2.0 = UCA0TXD, P2.1 = UCA0RXD */
    P2SEL1 |= BIT0 | BIT1;
    P2SEL0 &= ~(BIT0 | BIT1);

    /* Configure DCO to 1 MHz */
    CSCTL0_H = CSKEY_H;
    CSCTL1 = DCOFSEL_0; /* 1 MHz DCO */
    CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3 = DIVA__1 | DIVS__1 | DIVM__1;
    CSCTL0_H = 0;

    /* Configure eUSCI_A0 for 9600 baud, 1 MHz SMCLK */
    UCA0CTLW0 = UCSWRST; /* Hold in reset */
    UCA0CTLW0 |= UCSSEL__SMCLK;
    UCA0BRW = 6;                           /* 1000000 / 9600 = 104.17 */
    UCA0MCTLW = UCOS16 | UCBRF_8 | 0x2000; /* Oversampling, see UG Table */
    UCA0CTLW0 &= ~UCSWRST;                 /* Release reset */
}

/* Direct UART character/string output (no newlib stdio dependency) */
static void uart_putc(char c) {
    while (!(UCA0IFG & UCTXIFG))
        ;
    UCA0TXBUF = c;
}

static void uart_puts(const char *s) {
    while (*s)
        uart_putc(*s++);
}

static void uart_put_u16(uint16_t val) {
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

/* ============================================================================
 * NVM Debug Counters (survive power cycles, incremented inline by
 * instrumented code via ADD.W instructions)
 * ============================================================================ */

__attribute__((section(".nvm"))) uint16_t cnt_boundary;
__attribute__((section(".nvm"))) uint16_t cnt_save_reg;
__attribute__((section(".nvm"))) uint16_t cnt_restore_reg;

/* ============================================================================
 * UART init (constructor) + Counter output (destructor)
 * ============================================================================ */

__attribute__((constructor)) void __rockclimb_debug_init(void) {
    uart_init();
}

__attribute__((destructor)) void __rockclimb_print_counts(void) {
    uart_puts("=== RockClimb Debug Counter Summary ===\r\n");
    uart_puts("  __region_boundary:    ");
    uart_put_u16(cnt_boundary);
    uart_puts("\r\n");
    uart_puts("  reg_saves:            ");
    uart_put_u16(cnt_save_reg);
    uart_puts("\r\n");
    uart_puts("  reg_restores:         ");
    uart_put_u16(cnt_restore_reg);
    uart_puts("\r\n");
    uart_puts("========================================\r\n");
}
