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

/* Checkpointed register values */
__attribute__((section(".nvm"))) uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS];

/* Current region ID */
__attribute__((section(".nvm"))) uint16_t __nvm_region_id;

/* Saved program counter */
__attribute__((section(".nvm"))) uint16_t __nvm_pc;

/* Saved stack pointer */
__attribute__((section(".nvm"))) uint16_t __nvm_sp;

/* __region_boundary is provided by rockclimb_boot.S */
