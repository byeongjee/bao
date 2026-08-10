/*
 * Shared boot macros for MSP430 checkpoint runtimes.
 *
 * Included by milp_boot.S, rockclimb_boot.S, schematic_boot.S.
 * Requires F_CPU to be defined via -D at assembly time.
 */

#if !defined(HALT_BOR) && !defined(HALT_LPM4) && !defined(HALT_SWBOR) && !defined(HALT_WAIT)
#error "Define one of HALT_BOR, HALT_LPM4, HALT_SWBOR, or HALT_WAIT"
#endif

#if (defined(HALT_BOR) + defined(HALT_LPM4) + defined(HALT_SWBOR) + defined(HALT_WAIT)) > 1
#error "Define only one halt mode"
#endif

#ifdef HALT_WAIT
    .extern wait_until_vcc_full
#endif

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
 * poison_bor_sram — Model volatile SRAM loss for BOR verification.
 *
 * Software-triggered BOR leaves SRAM intact on the FR5994. BOR is the
 * correctness mode, so erase the device SRAM range before restoring
 * checkpointed state regardless of whether debug counters are enabled.
 * SWBOR deliberately retains SRAM for continuous-power measurement.
 */
.macro poison_bor_sram
#if defined(HALT_BOR)
    mov.w   #__ram_start, R4
1:
    clr.w   0(R4)
    add.w   #2, R4
    cmp.w   #__ram_end, R4
    jne     1b
#endif
.endm

/*
 * poison_bor_gprs — Model volatile CPU state loss for BOR verification.
 *
 * PMMSWBOR resets the MSP430 but leaves R4-R15 unchanged on the FR5994.
 * That can hide missing checkpoint restores, so BOR mode clears them after
 * saving the resume PC and before algorithm-specific restoration. SWBOR
 * deliberately retains them for continuous-power measurement.
 */
.macro poison_bor_gprs
#if defined(HALT_BOR)
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
 * Halt mode selected by HALT_BOR / HALT_LPM4 / HALT_SWBOR / HALT_WAIT.
 * HALT_WAIT does not halt: it waits for a full capacitor and continues.
 */
.macro save_pc_sp_and_halt
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

#elif defined(HALT_WAIT)
    /* Real intermittent power: the checkpoint above is complete, so wait
       until the capacitor is full and continue into the next region
       without rebooting. A power failure during the wait recovers to
       this boundary. RockClimb inserts this call post-regalloc, where
       caller-saved registers and SR flags may be live — preserve them
       around the C call. */
    push    SR
    push    R11
    push    R12
    push    R13
    push    R14
    push    R15
    call    #wait_until_vcc_full
    pop     R15
    pop     R14
    pop     R13
    pop     R12
    pop     R11
    pop     SR

#else /* HALT_LPM4 */
    /* Enter LPM4 deep sleep (CPUOFF+OSCOFF+SCG0+SCG1 = bits 4-7 of SR).
       System powers off. On energy harvest, reset vector fires. */
    bis.w   #0xF0, SR
    nop
    /* If woken by interrupt (not full reboot), fall through to ret */

#endif /* BOR vs SWBOR vs WAIT vs LPM4 */
.endm

/*
 * zero_nvm_state — Clear __nvm_pc and __nvm_sp for fresh boot.
 */
.macro zero_nvm_state
    mov.w   #0, &__nvm_pc
    mov.w   #0, &__nvm_sp
.endm
