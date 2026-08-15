/*
 * RockClimb Runtime Library Implementation for MSP430FR5994
 *
 * All-FRAM operation: mutable data lives in FRAM, stack in FRAM.
 * Region boundaries save state and halt (deep sleep). On reboot,
 * boot.S recovers from NVM and resumes execution.
 *
 * __region_boundary is provided by rockclimb_boot.S (assembly).
 *
 * When compiled with -DDEVICE_DEBUG, also provides:
 *   - NVM debug counter (cnt_boundary)
 *   - NVM result storage (__nvm_result, __nvm_done)
 *   - UART output for profiling
 *   - debug_init() / debug_exit() API
 */

#include "rockclimb_runtime.h"

/* ============================================================================
 * NVM Storage Definitions (always present — used by boot.S for recovery)
 * ============================================================================ */

/* Checkpointed register values.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash (prevents FRAM garbage recovery crash). */
__attribute__((section(".nvm"))) uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS] = {0};

/* Saved program counter */
__attribute__((section(".nvm"))) uint16_t __nvm_pc = 0;

/* Saved stack pointer */
__attribute__((section(".nvm"))) uint16_t __nvm_sp = 0;

/* 1 while a region is executing; cleared once a boundary's checkpoint
   is committed. Boot code treats a reset with this flag set as an
   energy-budget violation (see check_region_violation in boot_common.inc). */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_in_region = 0;

/* Set by boot code when a reset arrived mid-region. Read by the host
   after every run; only reflashing clears it. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_violation = 0;

/* __region_boundary is provided by rockclimb_boot.S */

#include <msp430.h>
void bench_halt(void) {
    /* Run complete — a later reset (e.g. mspdebug attach) is not a
       mid-region death. */
    __nvm_in_region = 0;
    __bis_SR_register(LPM4_bits);
}

#ifdef DEVICE_DEBUG

#include "debug_common.h"

/* ============================================================================
 * NVM Debug Counters (survive power cycles, incremented inline by
 * instrumented code via 32-bit low-word plus carry propagation)
 * ============================================================================ */

__attribute__((section(".nvm"))) uint32_t cnt_boundary = 0;

/* cnt_recovery — incremented in assembly (recovery path in rockclimb_boot.S).
   One per recovery boot, i.e. per power failure resumed from a
   checkpoint (deaths during a boundary wait re-enter recovery and
   count again). */
__attribute__((section(".nvm"))) uint32_t cnt_recovery = 0;

/* NVM result storage — read by host via mspdebug md (bypasses UART) */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;

void debug_exit(int result) {
    debug_exit_begin(result);
    uart_puts("  __region_boundary:    ");
    uart_put_u32(cnt_boundary);
    uart_puts("\r\n");
    uart_puts("  recoveries:           ");
    uart_put_u32(cnt_recovery);
    uart_puts("\r\n");
    debug_exit_end();
}

#endif /* DEVICE_DEBUG */
