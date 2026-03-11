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
 */

#include "milp_runtime.h"

/* ============================================================================
 * NVM Storage Definitions
 * ============================================================================ */

/* Saved program counter.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash (prevents FRAM garbage recovery crash). */
__attribute__((section(".nvm"))) uint16_t __nvm_pc = 0;

/* Saved stack pointer */
__attribute__((section(".nvm"))) uint16_t __nvm_sp = 0;

/* ============================================================================
 * NVM Debug Counters
 * ============================================================================ */

__attribute__((section(".nvm"))) uint16_t cnt_boundary = 0;
__attribute__((section(".nvm"))) uint16_t cnt_save_vreg = 0;
__attribute__((section(".nvm"))) uint16_t cnt_restore_vreg = 0;
__attribute__((section(".nvm"))) uint16_t cnt_store_mem = 0;
__attribute__((section(".nvm"))) uint16_t cnt_restore_mem = 0;

/* __region_boundary is provided by milp_boot.S */
