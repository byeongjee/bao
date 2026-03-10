/*
 * RockClimb Runtime Library Implementation for MSP430FR5994
 *
 * All-FRAM operation: mutable data lives in FRAM, stack in FRAM.
 * Region boundaries save state and halt (deep sleep). On reboot,
 * boot.S recovers from NVM and resumes execution.
 *
 * __region_boundary is provided by rockclimb_boot.S (assembly).
 */

#include "rockclimb_runtime.h"

/* ============================================================================
 * NVM Storage Definitions
 * ============================================================================ */

/* Checkpointed register values.
   Explicit zero initializers force PROGBITS so the flash programmer
   writes zeros on first flash (prevents FRAM garbage recovery crash). */
__attribute__((section(".nvm"))) uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS] = {0};

/* Saved program counter */
__attribute__((section(".nvm"))) uint16_t __nvm_pc = 0;

/* Saved stack pointer */
__attribute__((section(".nvm"))) uint16_t __nvm_sp = 0;

/* __region_boundary is provided by rockclimb_boot.S */
