/*
 * MILP Boot/Recovery Assembly Routine for MSP430
 *
 * Placeholder boot hook mirroring the RockClimb integration pattern.
 * This code is placed in .crt_0250 to execute after data init but before
 * main() in the MSP430 GCC startup sequence.
 *
 * A future MILP intermittent-power runtime can extend this to detect recovery
 * state in NVM and jump to a saved PC.
 */

    .file   "milp_boot.S"

/* External symbols from milp_runtime.c */
    .extern __milp_nvm_region_id
    .extern __milp_nvm_pc
    .extern __milp_nvm_sp

/* Boot hook entry point (runs as part of CRT init flow). */
    .section .crt_0250, "ax", @progbits
    .global _milp_boot
    .type   _milp_boot, @function

_milp_boot:
    /* Placeholder: clear NVM markers so we always start fresh for now. */
    mov.w   #0, &__milp_nvm_region_id
    mov.w   #0, &__milp_nvm_pc
    mov.w   #0, &__milp_nvm_sp

    /* Fall through to next CRT section (do NOT use ret here). */

    .size   _milp_boot, .-_milp_boot

    .end
