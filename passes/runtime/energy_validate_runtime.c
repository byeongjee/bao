/*
 * Energy Validation Runtime
 *
 * Drop-in replacement for production checkpoint runtimes.
 * Each function subtracts its energy cost from the shared global
 * and checks/resets at checkpoint boundaries.
 */

#include <stdio.h>
#include <stdlib.h>

/* ============================================================================
 * Shared globals (defined by EnergyValidatorPass in the instrumented module;
 * we declare them extern here so the runtime can read/write them)
 * ============================================================================ */

extern double __ev_energy_remaining;
extern double __ev_capacity;
extern double __ev_E_pro;
extern double __ev_E_epi;
extern double __ev_reg_store;
extern double __ev_reg_restore;
extern double __ev_mem_store_per_byte;
extern double __ev_mem_restore_per_byte;
extern double __ev_epsilon;

/* ============================================================================
 * Violation handler
 * ============================================================================ */

void __energy_violation(const char *func, const char *block,
                        double energy, double capacity) {
    fprintf(stderr,
            "ENERGY VIOLATION at %s (%s): remaining=%.4f, capacity=%.2f\n",
            func, block, energy, capacity);
    abort();
}

/* ============================================================================
 * Verbose print (called when -validate-verbose is set)
 * ============================================================================ */

void __ev_verbose_print(const char *func, const char *block, double remaining) {
    fprintf(stderr, "[energy-validate] %s/%s: remaining=%.6f\n",
            func, block, remaining);
}

/* ============================================================================
 * MILP runtime API
 * ============================================================================ */

void __region_prologue(void) {
    /* Check: previous segment must have fit in the budget */
    if (__ev_energy_remaining < -__ev_epsilon) {
        __energy_violation("__region_prologue", "check",
                           __ev_energy_remaining, __ev_capacity);
    }
    /* Reset for new segment */
    __ev_energy_remaining = __ev_capacity;
    /* Subtract prologue cost */
    __ev_energy_remaining -= __ev_E_pro;
}

void __region_epilogue(void) {
    __ev_energy_remaining -= __ev_E_epi;
}

void __checkpoint_store_reg(int slot_id, long value) {
    (void)slot_id;
    (void)value;
    __ev_energy_remaining -= __ev_reg_store;
}

void __checkpoint_store_mem(void *nvm_dst, void *vm_src, int size) {
    (void)nvm_dst;
    (void)vm_src;
    __ev_energy_remaining -= __ev_mem_store_per_byte * size;
}

void __restore_reg(int slot_id, void *dest) {
    (void)slot_id;
    (void)dest;
    __ev_energy_remaining -= __ev_reg_restore;
}

void __restore_mem(void *vm_dst, void *nvm_src, int size) {
    (void)vm_dst;
    (void)nvm_src;
    __ev_energy_remaining -= __ev_mem_restore_per_byte * size;
}

/* ============================================================================
 * RockClimb runtime API
 * ============================================================================ */

void __rockclimb_check(void) {
    if (__ev_energy_remaining < -__ev_epsilon) {
        __energy_violation("__rockclimb_check", "check",
                           __ev_energy_remaining, __ev_capacity);
    }
    __ev_energy_remaining = __ev_capacity;
}

void __rockclimb_save_reg(int reg_id, int value) {
    (void)reg_id;
    (void)value;
    __ev_energy_remaining -= __ev_reg_store;
}

/* ============================================================================
 * Generic checkpoint API
 * ============================================================================ */

void checkpoint(void) {
    if (__ev_energy_remaining < -__ev_epsilon) {
        __energy_violation("checkpoint", "check",
                           __ev_energy_remaining, __ev_capacity);
    }
    __ev_energy_remaining = __ev_capacity;
}

void __checkpoint(void) {
    if (__ev_energy_remaining < -__ev_epsilon) {
        __energy_violation("__checkpoint", "check",
                           __ev_energy_remaining, __ev_capacity);
    }
    __ev_energy_remaining = __ev_capacity;
}
