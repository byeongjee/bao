/*
 * Energy Validation Runtime
 *
 * Drop-in replacement for production checkpoint runtimes.
 * Each checkpoint function subtracts its energy cost from a shared global
 * and checks/resets at checkpoint boundaries.
 *
 * Used with the energy-validate LLVM pass for dynamic energy validation.
 */

#ifndef ENERGY_VALIDATE_RUNTIME_H
#define ENERGY_VALIDATE_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Shared globals (emitted by EnergyValidatorPass, read here via extern)
 * ============================================================================ */

extern double __ev_energy_remaining;   /* Current energy budget */
extern double __ev_capacity;           /* Effective capacity (E_buf or E_safe) */
extern double __ev_E_pro;              /* Prologue cost */
extern double __ev_E_epi;              /* Epilogue cost */
extern double __ev_reg_store;          /* Per-register store cost */
extern double __ev_reg_restore;        /* Per-register restore cost */
extern double __ev_mem_store_per_byte; /* Per-byte memory store cost */
extern double __ev_mem_restore_per_byte; /* Per-byte memory restore cost */
extern double __ev_nvm_access_penalty; /* Per-NVM-access penalty */
extern double __ev_epsilon;            /* Floating-point tolerance */

/* ============================================================================
 * Violation handler
 * ============================================================================ */

void __energy_violation(const char *func, const char *block,
                        double energy, double capacity);

/* ============================================================================
 * Verbose print (optional, enabled by -validate-verbose)
 * ============================================================================ */

void __ev_verbose_print(const char *func, const char *block, double remaining);

/* ============================================================================
 * MILP runtime API
 * ============================================================================ */

void __region_prologue(void);
void __region_epilogue(void);
void __checkpoint_store_reg(int slot_id, long value);
void __checkpoint_store_mem(void *nvm_dst, void *vm_src, int size);
void __restore_reg(int slot_id, void *dest);
void __restore_mem(void *vm_dst, void *nvm_src, int size);

/* ============================================================================
 * RockClimb runtime API
 * ============================================================================ */

void __rockclimb_check(void);
void __rockclimb_save_reg(int reg_id, int value);

/* ============================================================================
 * Generic checkpoint API
 * ============================================================================ */

void checkpoint(void);
void __checkpoint(void);

#ifdef __cplusplus
}
#endif

#endif /* ENERGY_VALIDATE_RUNTIME_H */
