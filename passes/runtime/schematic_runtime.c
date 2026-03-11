/*
 * SCHEMATIC Runtime Library Implementation for MSP430FR5994
 *
 * All-FRAM operation: mutable data lives in FRAM, stack in FRAM.
 * Region boundaries bulk-save registers and halt (deep sleep). On reboot,
 * schematic_boot.S recovers from NVM and resumes execution.
 *
 * __region_boundary is provided by schematic_boot.S (assembly).
 */

#include "schematic_runtime.h"

/* ============================================================================
 * NVM Storage Definitions
 * ============================================================================ */

/* Checkpointed register values.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash (prevents FRAM garbage recovery crash). */
__attribute__((section(".nvm"))) uint16_t __nvm_regs[SCHEMATIC_MAX_REGS] = {0};

/* Saved program counter */
__attribute__((section(".nvm"))) uint16_t __nvm_pc = 0;

/* Saved stack pointer */
__attribute__((section(".nvm"))) uint16_t __nvm_sp = 0;

/* ============================================================================
 * NVM Debug Counters
 * ============================================================================ */

__attribute__((section(".nvm"))) uint16_t cnt_boundary = 0;
__attribute__((section(".nvm"))) uint16_t cnt_save_reg = 0;
__attribute__((section(".nvm"))) uint16_t cnt_restore_reg = 0;
__attribute__((section(".nvm"))) uint16_t cnt_store_mem = 0;
__attribute__((section(".nvm"))) uint16_t cnt_restore_mem = 0;

/* __region_boundary is provided by schematic_boot.S */
