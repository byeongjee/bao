/*
 * RockClimb Boot/Recovery Assembly Routine for MSP430
 *
 * This file implements:
 * 1. Early boot recovery: checks NVM state and restores registers + PC
 * 2. __region_boundary: saves state and halts (deep sleep) at region boundaries
 */

    .file   "rockclimb_boot.S"

#include "boot_common.inc"

/*
 * External symbols from rockclimb_runtime.c
 */
    .extern __nvm_regs
    .extern __nvm_pc
    .extern __nvm_sp

/*
 * Boot/Recovery entry point
 *
 * Placed in .crt_0010 — runs BEFORE data/BSS initialization.
 * On recovery, jumps to saved PC immediately, preserving FRAM state.
 * On fresh boot, falls through to CRT data init.
 */
    .section .crt_0010, "ax", @progbits
    .global _rockclimb_boot
    .type   _rockclimb_boot, @function

_rockclimb_boot:
    park_if_done
    check_region_violation

    /* Check if this is a recovery boot: nvm_pc != 0 */
    mov.w   &__nvm_pc, R14
    tst.w   R14
    jz      _normal_boot

    /* This is a recovery boot - restore all state from NVM */

_do_recovery:
    hw_init
    poison_bor_sram

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

    /*
     * Restore all general-purpose registers R4-R15 from NVM.
     * Slot layout matches deterministic regId mapping:
     *   __nvm_regs[0]=R4, [1]=R5, ..., [11]=R15
     * Each slot is 2 bytes (uint16_t).
     */
    mov.w   &__nvm_regs+0,  R4
    mov.w   &__nvm_regs+2,  R5
    mov.w   &__nvm_regs+4,  R6
    mov.w   &__nvm_regs+6,  R7
    mov.w   &__nvm_regs+8,  R8
    mov.w   &__nvm_regs+10, R9
    mov.w   &__nvm_regs+12, R10
    mov.w   &__nvm_regs+14, R11
    mov.w   &__nvm_regs+16, R12
    mov.w   &__nvm_regs+18, R13
    mov.w   &__nvm_regs+20, R14
    mov.w   &__nvm_regs+22, R15

#ifdef DEVICE_DEBUG
    /* Per-register restore counting is disabled to keep debug builds small.
       The distributed save/restore data still lives in FRAM; only the
       counter update is omitted. */
#endif

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

    .size   _rockclimb_boot, .-_rockclimb_boot


/*
 * __region_boundary — Save state and halt at region boundaries
 *
 * Called at the start of each energy region (except the entry block).
 * Saves the return address (= region body start) and SP to NVM, then
 * triggers BOR (or simulates one, or waits for a full capacitor —
 * per halt mode). After a reboot, the reset vector fires, CRT runs,
 * and boot recovery (above) resumes from saved state.
 *
 * RockClimb's machine pass inserts per-register saves distributed
 * at definition points — no bulk-save needed here.
 */
    .section .text
    .global __region_boundary
    .type   __region_boundary, @function

__region_boundary:
#ifdef DEVICE_DEBUG
    /* Preserve SR flags for the wait-mode continue path. */
    push    SR
    add_u32_to_nvm_counter cnt_boundary, 1
    pop     SR
#endif

    save_pc_sp_and_halt

    ret

    .size   __region_boundary, .-__region_boundary


/*
 * Save current context to NVM (bulk save alternative)
 */
    .section .text
    .global _rockclimb_save_context
    .type   _rockclimb_save_context, @function

_rockclimb_save_context:
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

    mov.w   SP, &__nvm_sp
    mov.w   0(SP), &__nvm_pc

    ret

    .size   _rockclimb_save_context, .-_rockclimb_save_context

    .end
