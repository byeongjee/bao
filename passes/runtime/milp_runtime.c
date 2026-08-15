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
 *   - NVM debug counters (cnt_boundary, cnt_store_mem, etc.)
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

/* 1 while a region is executing; cleared once a boundary's checkpoint
   is committed. Boot code treats a reset with this flag set as an
   energy-budget violation (see check_region_violation in boot_common.inc). */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_in_region = 0;

/* Set by boot code when a reset arrived mid-region. Read by the host
   after every run; only reflashing clears it. */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_violation = 0;

/* Set once the run completes; park_if_done in boot code then freezes
   every later boot so NVM state survives readback. Non-debug builds
   set it via bench_commit_done(), debug builds via debug_exit_commit(). */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;

/* cnt_recovery — incremented in assembly (recovery path in milp_boot.S).
   One per recovery boot, i.e. per power failure resumed from a
   checkpoint (deaths during a boundary wait re-enter recovery and
   count again). Unconditional: recovery counting must work in
   non-debug builds, which are the ones run under intermittent power. */
__attribute__((section(".nvm"))) uint32_t cnt_recovery = 0;

/* __region_boundary is provided by milp_boot.S */

/* Halt CPU after benchmark completes.
   Prevents post-main restarts from producing spurious GPIO edges
   that inflate Saleae timing measurements. */
#include <msp430.h>
void bench_halt(void) {
    /* Run complete — a later reset (e.g. mspdebug attach) is not a
       mid-region death. */
    __nvm_in_region = 0;
    __bis_SR_register(LPM4_bits);
}

/* Called by non-debug BENCH_EXIT before the end pulse (see benchmark.h). */
void bench_commit_done(void) {
    __nvm_in_region = 0;
    __nvm_done = 1;
}

#ifdef DEVICE_DEBUG

#include "debug_common.h"

/* ============================================================================
 * NVM Debug Counters
 *
 * cnt_boundary — incremented in assembly (__region_boundary in milp_boot.S)
 * cnt_store_mem, cnt_restore_mem — incremented at IR level by instrumenter
 * ============================================================================ */

__attribute__((section(".nvm"))) uint32_t cnt_boundary = 0;
__attribute__((section(".nvm"))) uint32_t cnt_store_mem = 0;
__attribute__((section(".nvm"))) uint32_t cnt_restore_mem = 0;

/* NVM result storage — read by host via mspdebug md (bypasses UART) */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;

void debug_exit(int result) {
    debug_exit_begin(result);
    uart_puts("  __region_boundary:    ");
    uart_put_u32(cnt_boundary);
    uart_puts("\r\n");
    uart_puts("  mem_stores:           ");
    uart_put_u32(cnt_store_mem);
    uart_puts("\r\n");
    uart_puts("  mem_restores:         ");
    uart_put_u32(cnt_restore_mem);
    uart_puts("\r\n");
    uart_puts("  recoveries:           ");
    uart_put_u32(cnt_recovery);
    uart_puts("\r\n");
    debug_exit_end();
}

#endif /* DEVICE_DEBUG */
