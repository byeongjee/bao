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

/* Halt CPU after benchmark completes.
   Prevents post-main restarts from producing spurious GPIO edges
   that inflate Saleae timing measurements. */
#include <msp430.h>
void bench_halt(void) {
    __bis_SR_register(LPM4_bits);
}

#ifdef DEVICE_DEBUG

#include "debug_common.h"

/* ============================================================================
 * NVM Debug Counters
 *
 * cnt_boundary — incremented in assembly (__region_boundary in milp_boot.S)
 * cnt_save_vreg, cnt_restore_vreg — incremented at IR level by instrumenter
 * cnt_store_mem, cnt_restore_mem — incremented at IR level by instrumenter
 *
 * "vreg" counters count IR-level value saves, not physical register saves.
 * ============================================================================ */

__attribute__((section(".nvm"))) uint32_t cnt_boundary = 0;
__attribute__((section(".nvm"))) uint32_t cnt_save_vreg = 0;
__attribute__((section(".nvm"))) uint32_t cnt_restore_vreg = 0;
__attribute__((section(".nvm"))) uint32_t cnt_store_mem = 0;
__attribute__((section(".nvm"))) uint32_t cnt_restore_mem = 0;

/* NVM result storage — read by host via mspdebug md (bypasses UART) */
__attribute__((section(".nvm"))) volatile uint16_t __nvm_result = 0;
__attribute__((section(".nvm"))) volatile uint16_t __nvm_done = 0;

void debug_exit(int result) {
    debug_exit_begin(result);
    uart_puts("  __region_boundary:    ");
    uart_put_u32(cnt_boundary);
    uart_puts("\r\n");
    uart_puts("  vreg_saves:           ");
    uart_put_u32(cnt_save_vreg);
    uart_puts("\r\n");
    uart_puts("  vreg_restores:        ");
    uart_put_u32(cnt_restore_vreg);
    uart_puts("\r\n");
    uart_puts("  mem_stores:           ");
    uart_put_u32(cnt_store_mem);
    uart_puts("\r\n");
    uart_puts("  mem_restores:         ");
    uart_put_u32(cnt_restore_mem);
    uart_puts("\r\n");
    debug_exit_end();
}

#endif /* DEVICE_DEBUG */
