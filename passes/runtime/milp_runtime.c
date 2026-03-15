/*
 * MILP Runtime Library Implementation for MSP430FR5994
 *
 * MILP stack is in SRAM (unlike RockClimb/SCHEMATIC which use all-FRAM),
 * requiring a separate linker script (milp_msp430fr5994.ld).
 *
 * Region boundaries save only PC + SP (no register bulk-save needed).
 * The MILP instrumenter commits/restores all live state at IR level.
 *
 * __region_boundary is provided by milp_boot.S (assembly).
 *
 * When compiled with -DDEVICE_DEBUG, also provides:
 *   - NVM debug counters (cnt_boundary, cnt_save_vreg, etc.)
 *   - NVM result storage (__nvm_result, __nvm_done)
 *   - UART output for profiling
 *   - debug_init() / debug_exit() API
 */

#include "milp_runtime.h"

/* ============================================================================
 * NVM Storage Definitions (always present — used by boot.S for recovery)
 * ============================================================================ */

/* Saved program counter.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash (prevents FRAM garbage recovery crash). */
__attribute__((section(".nvm"))) uint16_t __nvm_pc = 0;

/* Saved stack pointer */
__attribute__((section(".nvm"))) uint16_t __nvm_sp = 0;

/* __region_boundary is provided by milp_boot.S */

#ifdef DEVICE_DEBUG

#include "benchmark.h"
#include <msp430.h>

/* ============================================================================
 * NVM Debug Counters
 *
 * cnt_boundary — incremented in assembly (__region_boundary in milp_boot.S)
 * cnt_save_vreg, cnt_restore_vreg — incremented at IR level by instrumenter
 * cnt_store_mem, cnt_restore_mem — incremented at IR level by instrumenter
 *
 * "vreg" counters count IR-level value saves, not physical register saves.
 * ============================================================================ */

__attribute__((section(".nvm"))) uint16_t cnt_boundary = 0;
__attribute__((section(".nvm"))) uint16_t cnt_save_vreg = 0;
__attribute__((section(".nvm"))) uint16_t cnt_restore_vreg = 0;
__attribute__((section(".nvm"))) uint16_t cnt_store_mem = 0;
__attribute__((section(".nvm"))) uint16_t cnt_restore_mem = 0;

/* NVM result storage — read by host via mspdebug md (bypasses UART) */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;

/* ============================================================================
 * UART Setup for MSP430FR5994
 *
 * Uses eUSCI_A0 at 9600 baud (assuming 1 MHz SMCLK from DCO).
 * ============================================================================ */

static void uart_init(void) {
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
 * Debug API (called explicitly from benchmarks via BENCH_INIT/BENCH_EXIT)
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

    /* UART output kept for manual debugging */
    uart_init(); /* Re-init UART — may be uninitialized after BOR/LPM4 recovery */
    uart_puts("RESULT: ");
    uart_put_u16((uint16_t)result);
    uart_puts("\r\n");
    uart_puts("=== Debug Counter Summary ===\r\n");
    uart_puts("  __region_boundary:    ");
    uart_put_u16(cnt_boundary);
    uart_puts("\r\n");
    uart_puts("  vreg_saves:           ");
    uart_put_u16(cnt_save_vreg);
    uart_puts("\r\n");
    uart_puts("  vreg_restores:        ");
    uart_put_u16(cnt_restore_vreg);
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

#endif /* DEVICE_DEBUG */
