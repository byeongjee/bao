/*
 * SCHEMATIC Debug Counters Runtime (MSP430)
 *
 * Links ALONGSIDE schematic_boot.S + schematic_runtime.c (not replacing them).
 * Provides UART output for profiling.
 *
 * Counter globals are defined in schematic_runtime.c (extern here):
 *   cnt_boundary, cnt_save_reg, cnt_restore_reg — incremented in assembly
 *   cnt_store_mem, cnt_restore_mem — incremented at IR level by instrumenter
 *
 * Benchmarks call debug_init() at start of main() and debug_exit()
 * at end. Guarded by DEBUG_COUNTERS macro — compiles to no-ops without it.
 * Constructor/destructor attributes are broken on msp430-elf-gcc 9.3.1.
 *
 * Compile with: msp430-elf-gcc -mmcu=MSP430FR5994 -msmall -O2
 */

#include <msp430.h>
#include <stdint.h>

#include "debug_counters.h"

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
 * NVM Debug Counters (defined in schematic_runtime.c, extern here)
 * ============================================================================ */

extern uint16_t cnt_boundary;
extern uint16_t cnt_save_reg;
extern uint16_t cnt_restore_reg;
extern uint16_t cnt_store_mem;
extern uint16_t cnt_restore_mem;

/* NVM result storage — read by host via mspdebug md (bypasses UART) */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;

/* ============================================================================
 * Breakpoint target for host-side NVM readback.
 *
 * Called after NVM values are written. The host sets a hardware breakpoint
 * at this function's address so that mspdebug "run" returns, allowing
 * subsequent "md" commands to read NVM in the same session.
 * ============================================================================ */

__attribute__((noinline, used)) void __nvm_breakpoint(void) {
    __asm__ volatile("" ::: "memory");
}

/* ============================================================================
 * Debug API (called explicitly from benchmarks)
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
    uart_init();
    uart_puts("BOOT\r\n");
}

void debug_exit(int result) {
    /* Store result in NVM for host-side reading via mspdebug md */
    __nvm_result = (uint16_t)result;
    __nvm_done = 1;
    __nvm_breakpoint(); /* Host sets HW breakpoint here to stop "run" */

    /* UART output kept for manual debugging */
    uart_init(); /* Re-init UART — may be uninitialized after BOR/LPM4 recovery */
    uart_puts("RESULT: ");
    uart_put_u16((uint16_t)result);
    uart_puts("\r\n");
    uart_puts("=== Debug Counter Summary ===\r\n");
    uart_puts("  __region_boundary:    ");
    uart_put_u16(cnt_boundary);
    uart_puts("\r\n");
    uart_puts("  reg_saves:            ");
    uart_put_u16(cnt_save_reg);
    uart_puts("\r\n");
    uart_puts("  reg_restores:         ");
    uart_put_u16(cnt_restore_reg);
    uart_puts("\r\n");
    uart_puts("  mem_stores:           ");
    uart_put_u16(cnt_store_mem);
    uart_puts("\r\n");
    uart_puts("  mem_restores:         ");
    uart_put_u16(cnt_restore_mem);
    uart_puts("\r\n");
    uart_puts("\n" DEBUG_END_MARKER "\r\n");

    /* Halt CPU — unblocks mspdebug "run" command for NVM readback */
    __bis_SR_register(LPM4_bits);
}
