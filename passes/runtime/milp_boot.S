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
 *
 * Recovery is placed in .crt_0010 (before data/BSS init) so that on recovery
 * we jump to saved PC before CRT reinitializes .data/.bss, preserving FRAM
 * contents.
 *
 * MSP430 GCC CRT sections:
 *   .crt_0000-.crt_0099: Before BSS clear
 *   .crt_0100-.crt_0199: Before data init
 *   .crt_0200-.crt_0299: After data init, before constructors
 *   .crt_0300-.crt_0399: Constructors
 *   .crt_0400-.crt_0499: After constructors, before main
 */

    .file   "milp_boot.S"

/*
 * External symbols from milp_runtime.c
 */
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
    .global _milp_boot
    .type   _milp_boot, @function

_milp_boot:
    /* Check if this is a recovery boot: nvm_pc != 0 */
    mov.w   &__nvm_pc, R14
    tst.w   R14
    jz      _normal_boot

    /* This is a recovery boot - restore PC + SP from NVM */

_do_recovery:
    /* Disable watchdog — reboot resets WDT to active state */
    mov.w   #0x5A80, &0x015C

    /* R14 holds saved PC. Clear recovery marker first to prevent
       infinite recovery loops on next reboot. */
    mov.w   #0, &__nvm_pc

    /* Restore SP */
    mov.w   &__nvm_sp, SP

    /* Push saved PC onto restored stack (for ret-based jump) */
    push    R14

    /* No register restore needed — MILP commits/restores all live state
       at IR level. After regalloc, these become physical register
       saves/restores automatically. */

    /* Jump to saved PC (pops from stack).
       After ret: PC = saved PC, SP = __nvm_sp (push -2, ret +2 cancel). */
    ret

_normal_boot:
    /*
     * Normal (fresh) boot - initialize NVM state and continue to CRT.
     * Handles the case where FRAM contains garbage on first power-up.
     */

    /* Disable watchdog timer (WDTCTL = WDTPW | WDTHOLD) */
    mov.w   #0x5A80, &0x015C

    mov.w   #0, &__nvm_pc
    mov.w   #0, &__nvm_sp

    /* Fall through to next CRT section (do NOT use ret here) */

    .size   _milp_boot, .-_milp_boot


/*
 * __region_boundary — Save PC + SP and halt at region boundaries
 *
 * Called at the start of each energy region (except the entry block).
 * Saves the return address (= region body start) and SP to NVM,
 * then enters deep sleep (LPM4) or triggers BOR. The system powers off.
 * On next power-on, the reset vector fires, CRT runs, and boot
 * recovery (above) resumes from saved state.
 *
 * Unlike RockClimb/SCHEMATIC, no register bulk-save is needed here.
 * MILP handles all live state at IR level.
 *
 * If woken from LPM4 by an interrupt (not a full reboot), just returns.
 */
    .section .text
    .global __region_boundary
    .type   __region_boundary, @function

__region_boundary:
#ifdef DEBUG_COUNTERS
    add.w   #1, &cnt_boundary
#endif

#if defined(MILP_HALT_BOR) || defined(MILP_HALT_LPM4)
    /* Save return address as recovery PC.
       0(SP) holds the return address pushed by CALL. */
    mov.w   0(SP), &__nvm_pc

    /* Save caller's SP (before CALL pushed return address) */
    mov.w   SP, &__nvm_sp
    add.w   #2, &__nvm_sp

#if defined(MILP_HALT_BOR)
    /* Software BOR: PMMCTL0 = PMMPW (0xA500) | PMMSWBOR (0x0004).
       CPU resets immediately, FRAM survives. On reboot, _milp_boot
       detects __nvm_pc != 0 and restores execution from saved state. */
    mov.w   #0xA504, &0x0120
    /* Unreachable — CPU resets before next instruction */

#else /* MILP_HALT_LPM4 */
    /* Enter LPM4 deep sleep (CPUOFF+OSCOFF+SCG0+SCG1 = bits 4-7 of SR).
       System powers off. On energy harvest, reset vector fires and
       _milp_boot recovers from NVM state. */
    bis.w   #0xF0, SR
    nop
    /* If woken by interrupt (not full reboot), fall through to ret */

#endif /* BOR vs LPM4 */

#else
    /* Debug/nop mode: no state save, no halt. Saving __nvm_pc here would
       leave a non-zero value in FRAM that triggers a spurious recovery on
       the next boot if the flash programmer doesn't reprogram the .nvm
       segment (FRAM retains data across power cycles and reflashes). */
#endif

    ret

    .size   __region_boundary, .-__region_boundary

    .end
