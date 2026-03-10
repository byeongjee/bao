/*
 * RockClimb Machine-Level Mock Checkpoint Counter Runtime (MSP430)
 *
 * MSP430-targeted version of rockclimb_mock_ckpt_counter.c.
 * Outputs counter summary over UART for serial readback.
 *
 * Each runtime function increments a static counter without performing
 * any NVM operations. At program exit (__attribute__((destructor))),
 * __rockclimb_print_counts() prints a summary over UART.
 *
 * Compile with: msp430-elf-gcc -mmcu=MSP430FR5994 -msmall -O2 -DDEBUG
 */

#include <msp430.h>
#include <stdint.h>
#include <stdio.h>

#include "rockclimb_runtime.h"

/* ============================================================================
 * UART Setup for MSP430FR5994
 *
 * Uses eUSCI_A0 at 9600 baud (assuming 1 MHz SMCLK from DCO).
 * Provides _write() syscall so printf() routes to UART.
 * ============================================================================ */

static void uart_init(void) {
    /* Stop watchdog */
    WDTCTL = WDTPW | WDTHOLD;

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

/* newlib _write syscall: route stdout/stderr to UART */
int _write(int fd, const char *buf, int len) {
    (void)fd;
    for (int i = 0; i < len; i++) {
        while (!(UCA0IFG & UCTXIFG))
            ;
        UCA0TXBUF = buf[i];
    }
    return len;
}

/* ============================================================================
 * NVM variable definitions (satisfy extern declarations in header)
 * ============================================================================ */

uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS];
uint16_t __nvm_region_id;
uint16_t __nvm_pc;
uint16_t __nvm_sp;
volatile uint16_t __rockclimb_vmax_threshold;

/* ============================================================================
 * Counters
 * ============================================================================ */

static uint32_t cnt_check;
static uint32_t cnt_save_reg;
static uint32_t cnt_region_prologue;
static uint32_t cnt_region_epilogue;
static uint32_t cnt_checkpoint_store_reg;
static uint32_t cnt_init;
static uint32_t cnt_is_recovery;
static uint32_t cnt_recover;

/* ============================================================================
 * Runtime API (counter stubs)
 * ============================================================================ */

void __rockclimb_check(void) {
    cnt_check++;
}

void __rockclimb_save_reg(void) {
    cnt_save_reg++;
}

void __region_prologue(void) {
    cnt_region_prologue++;
}

void __region_epilogue(void) {
    cnt_region_epilogue++;
}

void __checkpoint_store_reg(int32_t slot_id, int64_t value) {
    (void)slot_id;
    (void)value;
    cnt_checkpoint_store_reg++;
}

void __loop_tripcount(int max_iterations) {
    (void)max_iterations;
}

void __rockclimb_init(void) {
    uart_init();
    cnt_init++;
}

uint16_t __rockclimb_is_recovery(void) {
    cnt_is_recovery++;
    return 0;
}

void __rockclimb_recover(void) {
    cnt_recover++;
}

/* ============================================================================
 * Counter output (destructor + public symbol for explicit call)
 * ============================================================================ */

__attribute__((destructor)) void __rockclimb_print_counts(void) {
    printf("=== RockClimb Checkpoint Counter Summary ===\n");
    printf("  __rockclimb_check:        %lu\n", (unsigned long)cnt_check);
    printf("  __rockclimb_save_reg:     %lu\n", (unsigned long)cnt_save_reg);
    printf("  __region_prologue:        %lu\n", (unsigned long)cnt_region_prologue);
    printf("  __region_epilogue:        %lu\n", (unsigned long)cnt_region_epilogue);
    printf("  __checkpoint_store_reg:   %lu\n", (unsigned long)cnt_checkpoint_store_reg);
    printf("  __rockclimb_init:         %lu\n", (unsigned long)cnt_init);
    printf("  __rockclimb_is_recovery:  %lu\n", (unsigned long)cnt_is_recovery);
    printf("  __rockclimb_recover:      %lu\n", (unsigned long)cnt_recover);
    printf("=============================================\n");
}
