/*
 * MILP Runtime Library (MSP430FR5994)
 *
 * Placeholder implementation that satisfies linking for instrumented programs.
 * A real intermittent-power runtime can later use the reserved NVM state and
 * the boot hook in milp_boot.S.
 */

#include "milp_runtime.h"

/* The MILP runtime is primarily built with the MSP430 toolchain. When building
 * on non-MSP430 hosts (e.g., for editors/LSP), omit MSP430-specific section
 * placement attributes. */
#if defined(__MSP430__)
#define MILP_NVM __attribute__((section(".nvm")))
#else
#define MILP_NVM
#endif

/* Reserved NVM state (mirrors RockClimb layout style; not used yet). */
MILP_NVM
uint16_t __milp_nvm_region_id;

MILP_NVM
uint16_t __milp_nvm_pc;

MILP_NVM
uint16_t __milp_nvm_sp;

/* Room for future register/context storage (R4-R15 = 12 regs). */
MILP_NVM
uint16_t __milp_nvm_regs[12];

void __region_prologue(void) {
    /* TODO */
}

void __region_epilogue(void) {
    /* TODO */
}

void __checkpoint_store_reg(int32_t slot_id, int64_t value) {
    (void)slot_id;
    (void)value;
    /* TODO */
}

void __checkpoint_store_mem(void *nvm_dst, void *vm_src, int32_t size) {
    (void)nvm_dst;
    (void)vm_src;
    (void)size;
    /* TODO */
}

void __restore_reg(int32_t slot_id, void *dest) {
    (void)slot_id;
    (void)dest;
    /* TODO */
}

void __restore_mem(void *vm_dst, void *nvm_src, int32_t size) {
    (void)vm_dst;
    (void)nvm_src;
    (void)size;
    /* TODO */
}
