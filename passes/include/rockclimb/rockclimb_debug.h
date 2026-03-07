/*
 * RockClimb Debug Library for MSP430
 *
 * Provides UART-based debugging for verifying checkpoint/recovery behavior.
 *
 * Usage:
 *   Compile with -DDEBUG for UART printf output
 *   Compile without -DDEBUG for no debug output (production)
 */

#ifndef ROCKCLIMB_DEBUG_H
#define ROCKCLIMB_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================
 */

#ifndef CLOCK_HZ
#define CLOCK_HZ 16000000UL
#endif

#ifndef BAUD
#define BAUD 9600
#endif

/* ============================================================================
 * Debug Mode (when DEBUG is defined)
 * ============================================================================
 */

#ifdef DEBUG

#include <msp430.h>
#include <stdio.h>

/* __delay_cycles is a GCC built-in intrinsic, not available in Clang.
 * Provide a simple loop-based implementation for Clang. */
#ifdef __clang__
static inline void __delay_cycles(unsigned long __n) {
    while (__n > 0) {
        __asm__ volatile("nop");
        __n--;
    }
}
#endif

#define DEBUG_PRINT(...) printf(__VA_ARGS__)
#define DEBUG_CHECKPOINT(id) printf("[CKPT] Saving region %d\n", (int)(id))
#define DEBUG_RESTORE(id) printf("[RESTORE] Restoring region %d\n", (int)(id))
#define DEBUG_REGION(id) printf("[REGION] Entering region %d\n", (int)(id))
#define DEBUG_VAR(name, val) printf("[VAR] %s = %d\n", (name), (int)(val))
#define DEBUG_NVM_WRITE(name, val) printf("[NVM] %s <- %d\n", (name), (int)(val))
#define DEBUG_NVM_READ(name, val) printf("[NVM] %s -> %d\n", (name), (int)(val))

/* Block until UART TX buffer is ready, then send character */
static inline void uart_putc_block(char c) {
    while (!(UCA0IFG & UCTXIFG)) { /* wait */
    }
    UCA0TXBUF = (uint8_t)c;
}

/* Initialize UART A0 at configured baud rate (16 MHz SMCLK) */
static inline void uart_init(void) {
    P2SEL0 &= ~(BIT0 | BIT1);
    P2SEL1 |= (BIT0 | BIT1);

    UCA0CTLW0 = UCSWRST | UCSSEL__SMCLK;

#if BAUD == 9600
    UCA0BRW = 104;
    UCA0MCTLW = UCOS16 | UCBRF_3 | (0x00 << 8);
#elif BAUD == 115200
    UCA0BRW = 8;
    UCA0MCTLW = UCOS16 | UCBRF_11 | (0x00 << 8);
#else
#error "Unsupported BAUD. Use 9600 or 115200."
#endif

    UCA0CTLW0 &= ~UCSWRST;
}

/* Clock setup for 16 MHz DCO (FR5994) */
static inline void clock_init(void) {
    CSCTL0_H = CSKEY_H;
    CSCTL1 = DCOFSEL_0;
    CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3 = DIVA__4 | DIVS__4 | DIVM__4;
    CSCTL1 = DCOFSEL_4 | DCORSEL;
    __delay_cycles(60);
    CSCTL3 = DIVA__1 | DIVS__1 | DIVM__1;
    CSCTL4 &= ~VLOOFF;
    CSCTL0_H = 0;
}

/* Full debug initialization */
static inline void debug_init(void) {
    WDTCTL = WDTPW | WDTHOLD;
    PM5CTL0 &= ~LOCKLPM5;

    clock_init();

    /* Short delay for clock to stabilize */
    __delay_cycles(100000);

    uart_init();

    /* Unbuffer stdout so prints appear immediately */
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Startup message */
    printf("\n\n=== RockClimb Debug Ready ===\n");
}

/* ============================================================================
 * Newlib syscalls for printf support
 * ============================================================================
 */
#include <reent.h>
#include <unistd.h>

/* Newlib syscall: printf -> _write/_write_r (override weak symbols) */
int _write(int fd, const void *buf, size_t n) {
    (void)fd;
    const char *p = (const char *)buf;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '\n')
            uart_putc_block('\r'); /* CRLF for terminals */
        uart_putc_block(p[i]);
    }
    return (int)n;
}

int _write_r(struct _reent *r, int fd, const void *buf, size_t n) {
    (void)r;
    return _write(fd, buf, n);
}

#else /* No DEBUG */

#define DEBUG_PRINT(...) ((void)0)
#define DEBUG_CHECKPOINT(id) ((void)0)
#define DEBUG_RESTORE(id) ((void)0)
#define DEBUG_REGION(id) ((void)0)
#define DEBUG_VAR(name, val) ((void)0)
#define DEBUG_NVM_WRITE(name, val) ((void)0)
#define DEBUG_NVM_READ(name, val) ((void)0)

static inline void debug_init(void) {}

#endif /* DEBUG */

#ifdef __cplusplus
}
#endif

#endif /* ROCKCLIMB_DEBUG_H */
