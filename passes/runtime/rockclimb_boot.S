/*
 * RockClimb Boot/Recovery Assembly Routine for MSP430
 *
 * This file implements the early boot sequence that handles:
 * 1. Checking if this is a recovery boot
 * 2. Restoring registers from NVM on recovery
 * 3. Jumping to the saved program counter
 *
 * This code should be placed in the .init section to execute
 * before main() during startup.
 */

    .file   "rockclimb_boot.S"

/*
 * External symbols from rockclimb_runtime.c
 */
    .extern __nvm_regs
    .extern __nvm_region_id
    .extern __nvm_pc
    .extern __nvm_sp

/*
 * Boot/Recovery entry point
 *
 * This is called early in the startup sequence.
 * It checks if we're recovering from a power failure and
 * restores state if necessary.
 */
    .section .init, "ax", @progbits
    .global _rockclimb_boot
    .type   _rockclimb_boot, @function

_rockclimb_boot:
    /* Check if this is a recovery boot by examining region_id and PC */
    /* Recovery condition: region_id > 0 AND nvm_pc != 0 */

    mov.w   &__nvm_region_id, R15
    tst.w   R15
    jz      _normal_boot          /* region_id == 0: fresh boot */

    mov.w   &__nvm_pc, R14
    tst.w   R14
    jz      _normal_boot          /* PC == 0: no recovery point saved */

    /* This is a recovery boot - restore all state from NVM */

_do_recovery:
    /*
     * Restore general-purpose registers R4-R15 from NVM.
     * MSP430 register usage convention:
     *   R0 (PC) - Program Counter
     *   R1 (SP) - Stack Pointer
     *   R2 (SR) - Status Register
     *   R3 (CG) - Constant Generator
     *   R4-R11  - Callee-saved registers
     *   R12-R15 - Caller-saved / argument registers
     */

    /* Restore R4-R11 (callee-saved) */
    mov.w   &__nvm_regs+0,  R4
    mov.w   &__nvm_regs+2,  R5
    mov.w   &__nvm_regs+4,  R6
    mov.w   &__nvm_regs+6,  R7
    mov.w   &__nvm_regs+8,  R8
    mov.w   &__nvm_regs+10, R9
    mov.w   &__nvm_regs+12, R10
    mov.w   &__nvm_regs+14, R11

    /* Restore R12-R14 (caller-saved / arguments) */
    mov.w   &__nvm_regs+16, R12
    mov.w   &__nvm_regs+18, R13
    mov.w   &__nvm_regs+20, R14
    /* R15 will be loaded last with region_id or preserved */

    /* Restore stack pointer */
    mov.w   &__nvm_sp, SP

    /*
     * Clear the recovery state to prevent infinite recovery loops.
     * Set PC to 0 so next boot is treated as fresh.
     */
    mov.w   #0, &__nvm_pc

    /*
     * Jump to the saved program counter.
     * The PC was saved at the start of a region, so execution
     * will resume from the beginning of that region.
     */
    mov.w   R14, PC           /* R14 still holds __nvm_pc value */

    /* Should never reach here */

_normal_boot:
    /*
     * Normal (fresh) boot - continue to standard C startup.
     * The startup code will eventually call main().
     */

    /*
     * Initialize NVM region_id to 0 for fresh boot.
     * This handles the case where FRAM contains garbage on first power-up.
     */
    mov.w   #0, &__nvm_region_id
    mov.w   #0, &__nvm_pc
    mov.w   #0, &__nvm_sp

    /* Return to continue normal startup sequence */
    ret

    .size   _rockclimb_boot, .-_rockclimb_boot


/*
 * Save current context to NVM
 *
 * This function saves all registers and the return address to NVM.
 * It should be called when the application wants to create a checkpoint
 * that can be resumed after power failure.
 *
 * Note: This is an alternative to distributed checkpointing where
 * all registers are saved at once at a specific point.
 */
    .section .text
    .global _rockclimb_save_context
    .type   _rockclimb_save_context, @function

_rockclimb_save_context:
    /* Save all general-purpose registers to NVM */
    mov.w   R4,  &__nvm_regs+0
    mov.w   R5,  &__nvm_regs+2
    mov.w   R6,  &__nvm_regs+4
    mov.w   R7,  &__nvm_regs+6
    mov.w   R8,  &__nvm_regs+8
    mov.w   R9,  &__nvm_regs+10
    mov.w   R10, &__nvm_regs+12
    mov.w   R11, &__nvm_regs+14
    mov.w   R12, &__nvm_regs+16
    mov.w   R13, &__nvm_regs+18
    mov.w   R14, &__nvm_regs+20
    mov.w   R15, &__nvm_regs+22

    /* Save stack pointer */
    mov.w   SP, &__nvm_sp

    /* Save return address (on stack) as PC */
    /* Return address is at 0(SP) */
    mov.w   0(SP), &__nvm_pc

    ret

    .size   _rockclimb_save_context, .-_rockclimb_save_context


/*
 * Comparator_A interrupt vector
 *
 * This ISR is triggered when the voltage comparator detects that
 * the supply voltage has risen above the V_max threshold.
 * It wakes the CPU from LPM4.
 */
    .section .vectors, "ax", @progbits
    .global __comparator_vector

__comparator_vector:
    /* Clear interrupt flag - handled in C ISR */
    /* Exit LPM4 */
    bic.w   #0xF0, 0(SP)      /* Clear LPM bits in saved SR */
    reti

    .end
