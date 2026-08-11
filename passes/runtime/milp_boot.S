/*
 * MILP Boot/Recovery Assembly Routine for MSP430
 *
 * This file implements:
 * 1. Early boot recovery: checks NVM state and restores PC + SP
 * 2. __region_boundary: saves PC + SP and halts (deep sleep) at region boundaries
 *
 * Key difference from RockClimb/SCHEMATIC: MILP does NOT bulk-save registers
 * in __region_boundary. The MILP instrumenter commits/restores all live state
 * (globals, allocas, SSA values) at IR level — after register allocation, these
 * IR-level stores/loads become physical register saves/restores automatically.
 */

    .file   "milp_boot.S"

#include "boot_common.inc"

/*
 * External symbols from milp_runtime.c
 */
    .extern __nvm_pc
    .extern __nvm_sp
    .extern cnt_recovery

/*
 * Boot/Recovery entry point
 *
 * Placed in .crt_0010 — runs BEFORE data/BSS initialization.
 * On recovery, jumps to saved PC immediately, preserving FRAM state.
 * On fresh boot, falls through to CRT data init.
 */
    .section .crt_0010, "ax", @progbits
    .global _milp_boot
    .type   _milp_boot, @function

_milp_boot:
    park_if_done
    check_region_violation

    /* Check if this is a recovery boot: nvm_pc != 0 */
    mov.w   &__nvm_pc, R14
    tst.w   R14
    jz      _normal_boot

    /* This is a recovery boot - restore PC + SP from NVM */

_do_recovery:
    hw_init
    poison_bor_sram

#ifdef DEVICE_DEBUG
    /* Count recovery boots — one per power failure resumed from. */
    inc.w   &cnt_recovery
    adc.w   &cnt_recovery+2
#endif

#ifdef HALT_WAIT
    /* Real power failure: the board woke as soon as VCC crossed the
       brownout level. Wait for a full capacitor before resuming.
       __nvm_pc stays set so a death during this wait re-enters
       recovery on the next boot. SP is the CRT-initialized stack;
       the call clobbers R11-R15, so reload the saved PC after. */
    call    #wait_until_vcc_full
    mov.w   &__nvm_pc, R14
#else
    /* R14 holds saved PC. Clear recovery marker first to prevent
       infinite recovery loops on next reboot. */
    mov.w   #0, &__nvm_pc
#endif

    /* Restore SP */
    mov.w   &__nvm_sp, SP

    /* Push saved PC onto restored stack (for ret-based jump) */
    push    R14

    poison_bor_gprs

    /* No register restore needed — MILP commits/restores all live state
       at IR level. After regalloc, these become physical register
       saves/restores automatically. */

    mark_in_region

    /* Jump to saved PC (pops from stack).
       After ret: PC = saved PC, SP = __nvm_sp (push -2, ret +2 cancel). */
    ret

_normal_boot:
    /*
     * Normal (fresh) boot - initialize NVM state and continue to CRT.
     * Handles the case where FRAM contains garbage on first power-up.
     */
    hw_init
    zero_nvm_state

#ifdef HALT_WAIT
    /* Fresh boot: wait for a full capacitor before the program starts.
       Nothing persistent has been written yet, so a death during this
       wait is simply another clean fresh boot. */
    call    #wait_until_vcc_full
#endif

    /* Entering the first region (CRT init + entry code count toward it). */
    mark_in_region

    /* Fall through to next CRT section (do NOT use ret here) */

    .size   _milp_boot, .-_milp_boot


/*
 * __region_boundary — Save PC + SP and halt at region boundaries
 *
 * Called at the start of each energy region (except the entry block).
 * Saves the return address (= region body start) and SP to NVM, then
 * triggers BOR (or simulates one, or waits for a full capacitor —
 * per halt mode). After a reboot, the reset vector fires, CRT runs,
 * and boot recovery (above) resumes from saved state.
 *
 * Unlike RockClimb/SCHEMATIC, no register bulk-save is needed here.
 * MILP handles all live state at IR level.
 */
    .section .text
    .global __region_boundary
    .type   __region_boundary, @function

__region_boundary:
#ifdef DEVICE_DEBUG
    push    SR
    add_u32_to_nvm_counter cnt_boundary, 1
    pop     SR
#endif

    save_pc_sp_and_halt

    ret

    .size   __region_boundary, .-__region_boundary

    .end
