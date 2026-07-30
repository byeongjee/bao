/*
 * Shared boot macros for MSP430 checkpoint runtimes.
 *
 * Included by milp_boot.S, rockclimb_boot.S, schematic_boot.S.
 * Requires F_CPU to be defined via -D at assembly time.
 */

/*
 * hw_init — Disable WDT, unlock GPIO, configure FRAM wait states + DCO.
 *
 * Must be called early in both recovery and normal boot paths.
 * F_CPU is a -D define from the build system.
 */
.macro hw_init
    /* Disable watchdog timer (WDTCTL = WDTPW | WDTHOLD) */
    mov.w   #0x5A80, &0x015C

    /* Unlock GPIO pins — LPM5 locks all I/O after every reset on FR5xx/6xx */
    bic.w   #0x0001, &0x0130

    /* Configure FRAM wait states + DCO */
#if F_CPU > 8000000
    mov.w   #0xA510, &0x0140    /* FRCTL0: 1 FRAM wait state (required >8 MHz) */
#endif
#if F_CPU == 16000000
    mov.b   #0xA5, &0x0161      /* CSCTL0_H = CSKEY_H */
    mov.w   #0x0048, &0x0162    /* CSCTL1 = DCORSEL | DCOFSEL_4 = 16 MHz */
    mov.b   #0x00, &0x0161      /* CSCTL0_H = 0 (lock) */
#elif F_CPU == 8000000
    mov.b   #0xA5, &0x0161
    mov.w   #0x000C, &0x0162    /* CSCTL1 = DCOFSEL_6 = 8 MHz */
    mov.b   #0x00, &0x0161
#endif
.endm

/*
 * poison_reset_sram — Model volatile SRAM loss for debug reset runs.
 *
 * Software-triggered resets leave SRAM intact on the FR5994.  Semantic
 * verification must not rely on that retention, so erase the device SRAM
 * range before restoring checkpointed state.  The linker scripts provide
 * the bounds; RockClimb and SCHEMATIC place mutable state in FRAM, so the
 * same operation is harmless for their recovery paths.
 */
.macro poison_reset_sram
#if (defined(HALT_BOR) || defined(HALT_SWBOR)) && defined(DEVICE_DEBUG)
    mov.w   #__ram_start, R4
1:
    clr.w   0(R4)
    add.w   #2, R4
    cmp.w   #__ram_end, R4
    jne     1b
#endif
.endm

/*
 * poison_reset_gprs — Model volatile CPU state loss for debug reset runs.
 *
 * PMMSWBOR resets the MSP430 but leaves R4-R15 unchanged on the FR5994.
 * That can hide missing checkpoint restores.  Device-debug builds are used
 * for semantic verification, so clear the registers after saving the resume
 * PC and before algorithm-specific restoration.  Non-debug benchmark timing
 * remains unmodified.
 */
.macro poison_reset_gprs
#if (defined(HALT_BOR) || defined(HALT_SWBOR)) && defined(DEVICE_DEBUG)
    clr     R4
    clr     R5
    clr     R6
    clr     R7
    clr     R8
    clr     R9
    clr     R10
    clr     R11
    clr     R12
    clr     R13
    clr     R14
    clr     R15
#endif
.endm

/*
 * add_u32_to_nvm_counter — Add an immediate to a uint32_t counter in .nvm.
 *
 * Counter layout is little-endian, two 16-bit words:
 *   low  = counter + 0
 *   high = counter + 2
 *
 * The low-word ADD updates carry in SR, then ADDC propagates it into
 * the high word. Callers that need original flags preserved should wrap
 * this macro with push/pop SR.
 */
.macro add_u32_to_nvm_counter counter, amount
    add.w   #\amount, &\counter
    addc.w  #0, &\counter+2
.endm

/*
 * save_pc_sp_and_halt — Save return address + caller SP to NVM, then halt.
 *
 * Expects __nvm_pc and __nvm_sp to be .extern'd by the caller.
 * Halt mode selected by HALT_BOR / HALT_LPM4 / HALT_SWBOR defines.
 * If none is defined, this is debug/nop mode (no save, no halt).
 */
.macro save_pc_sp_and_halt
#if defined(HALT_BOR) || defined(HALT_LPM4) || defined(HALT_SWBOR)
    /* Save return address as recovery PC.
       0(SP) holds the return address pushed by CALL. */
    mov.w   0(SP), &__nvm_pc

    /* Save caller's SP (before CALL pushed return address) */
    mov.w   SP, &__nvm_sp
    add.w   #2, &__nvm_sp

#if defined(HALT_BOR)
    /* Software BOR: PMMCTL0 = PMMPW (0xA500) | PMMSWBOR (0x0004).
       CPU resets immediately, FRAM survives. */
    mov.w   #0xA504, &0x0120
    /* Unreachable — CPU resets before next instruction */

#elif defined(HALT_SWBOR)
    /* Software-simulated reboot: jump directly to _start.
       FRAM state (including __nvm_pc) survives; boot code detects
       __nvm_pc != 0 and enters recovery, simulating a full power-loss cycle. */
    br      #_start
    /* Unreachable — jump is unconditional */

#else /* HALT_LPM4 */
    /* Enter LPM4 deep sleep (CPUOFF+OSCOFF+SCG0+SCG1 = bits 4-7 of SR).
       System powers off. On energy harvest, reset vector fires. */
    bis.w   #0xF0, SR
    nop
    /* If woken by interrupt (not full reboot), fall through to ret */

#endif /* BOR vs SWBOR vs LPM4 */

#else
    /* Debug/nop mode: no state save, no halt. Saving __nvm_pc here would
       leave a non-zero value in FRAM that triggers a spurious recovery on
       the next boot if the flash programmer doesn't reprogram the .nvm
       segment (FRAM retains data across power cycles and reflashes). */
#endif
.endm

/*
 * zero_nvm_state — Clear __nvm_pc and __nvm_sp for fresh boot.
 */
.macro zero_nvm_state
    mov.w   #0, &__nvm_pc
    mov.w   #0, &__nvm_sp
.endm
