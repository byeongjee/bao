/*
 * MILP Checkpoint Runtime (MSP430)
 *
 * Declares NVM storage for boot recovery and debug counters.
 *
 * Key difference from RockClimb/SCHEMATIC: no __nvm_regs array.
 * MILP commits/restores all live state at IR level — after register
 * allocation, these IR-level stores/loads become physical register
 * saves/restores automatically.
 *
 * Debug counters use "vreg" terminology because the counts are IR-level
 * value saves, not physical register saves.
 * TODO: Register spilling may cause divergence between IR-level vreg
 * counts and actual physical register save/restore counts.
 */

#ifndef MILP_RUNTIME_H
#define MILP_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Section attribute for NVM placement (ELF targets only) */
#if defined(__ELF__)
#define NVM_SECTION __attribute__((section(".nvm")))
#else
#define NVM_SECTION
#endif

/* Saved program counter for recovery */
extern uint16_t __nvm_pc NVM_SECTION;

/* Saved stack pointer for recovery */
extern uint16_t __nvm_sp NVM_SECTION;

#ifdef DEBUG_COUNTERS
/* NVM Debug Counters */
extern uint16_t cnt_boundary NVM_SECTION;
extern uint16_t cnt_save_vreg NVM_SECTION;
extern uint16_t cnt_restore_vreg NVM_SECTION;
extern uint16_t cnt_store_mem NVM_SECTION;
extern uint16_t cnt_restore_mem NVM_SECTION;
#endif

/**
 * Region boundary: save PC + SP and halt (deep sleep).
 *
 * Saves return address (region body start) and SP to NVM, then halts.
 * On reboot, milp_boot.S recovers from saved state.
 *
 * Provided by milp_boot.S (assembly).
 */
void __region_boundary(void);

#ifdef __cplusplus
}
#endif

#endif /* MILP_RUNTIME_H */
