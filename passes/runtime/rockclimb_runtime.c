/*
 * RockClimb Runtime Library Implementation for MSP430
 *
 * Implements PFI (Proactive Failure Immunity) from the RockClimb paper.
 * This file is intended to be compiled with MSP430 toolchain.
 */

#include "rockclimb_runtime.h"

/* Include MSP430 headers when compiling for that target */
#ifdef __MSP430__
#include <msp430.h>
#endif

/* ============================================================================
 * NVM Storage Definitions
 * ============================================================================ */

/* Checkpointed register values */
__attribute__((section(".nvm")))
uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS];

/* Current region ID */
__attribute__((section(".nvm")))
uint16_t __nvm_region_id;

/* Saved program counter */
__attribute__((section(".nvm")))
uint16_t __nvm_pc;

/* Saved stack pointer */
__attribute__((section(".nvm")))
uint16_t __nvm_sp;

/* Voltage threshold - configurable at runtime */
volatile uint16_t __rockclimb_vmax_threshold = ROCKCLIMB_DEFAULT_VMAX_THRESHOLD;

/* ============================================================================
 * Runtime Implementation
 * ============================================================================ */

void __rockclimb_check(void) {
#ifdef __MSP430__
    /*
     * MSP430 voltage check implementation using Comparator_A or ADC.
     * This implementation assumes Comparator_A module is available.
     *
     * PFI Algorithm:
     * 1. Configure comparator to check V_cap against V_max threshold
     * 2. If V_cap < V_max, enter low-power mode
     * 3. Comparator interrupt wakes CPU when V_cap >= V_max
     * 4. Increment region ID and continue
     */

    /* Enable comparator with internal reference */
    CACTL1 = CAON | CAREF_2 | CARSEL;  /* Enable, use 0.5*Vcc ref, select minus input */
    CACTL2 = P2CA0;                     /* Select CA0 as positive input (external capacitor) */

    /* Wait for comparator to settle */
    __delay_cycles(100);

    /* Check if voltage is below threshold */
    if (!(CACTL2 & CAOUT)) {
        /* Voltage too low - enter LPM4 and wait for charge */

        /* Configure comparator interrupt for rising edge (voltage increasing) */
        CACTL1 |= CAIE;  /* Enable interrupt */
        CACTL1 &= ~CAIES; /* Rising edge */

        /* Clear any pending interrupt */
        CACTL1 &= ~CAIFG;

        /* Enter LPM4 with interrupts enabled */
        /* CPU will wake when voltage comparator triggers */
        __bis_SR_register(LPM4_bits | GIE);

        /* Disable comparator interrupt after waking */
        CACTL1 &= ~CAIE;
    }

    /* Disable comparator to save power */
    CACTL1 = 0;

    /* Increment region ID */
    __nvm_region_id++;

#else
    /*
     * Non-MSP430 stub for testing/simulation.
     * In simulation, we just increment the region ID.
     */
    __nvm_region_id++;
#endif
}

void __rockclimb_save_reg(uint8_t reg_id, uint16_t value) {
    if (reg_id < ROCKCLIMB_MAX_REGS) {
        __nvm_regs[reg_id] = value;
    }
}

void __rockclimb_init(void) {
#ifdef __MSP430__
    /* Stop watchdog timer */
    WDTCTL = WDTPW | WDTHOLD;

    /* Check if this is a fresh boot or recovery */
    if (__nvm_region_id == 0xFFFF) {
        /* Fresh boot - FRAM/Flash erased state is typically 0xFFFF */
        __nvm_region_id = 0;
        __nvm_pc = 0;
        __nvm_sp = 0;

        /* Clear register storage */
        for (uint8_t i = 0; i < ROCKCLIMB_MAX_REGS; i++) {
            __nvm_regs[i] = 0;
        }
    }

    /* Configure unused GPIO to reduce power consumption */
    /* (Specific configuration depends on hardware) */

#else
    /* Non-MSP430 initialization */
    if (__nvm_region_id == 0xFFFF || __nvm_region_id == 0) {
        __nvm_region_id = 0;
        __nvm_pc = 0;
        __nvm_sp = 0;
        for (uint8_t i = 0; i < ROCKCLIMB_MAX_REGS; i++) {
            __nvm_regs[i] = 0;
        }
    }
#endif
}

uint16_t __rockclimb_is_recovery(void) {
    /* If region_id > 0 and PC is set, this is a recovery */
    return (__nvm_region_id > 0 && __nvm_pc != 0);
}

void __rockclimb_recover(void) {
#ifdef __MSP430__
    /*
     * Recovery sequence:
     * 1. Restore stack pointer
     * 2. Restore general-purpose registers from NVM
     * 3. Jump to saved PC
     *
     * This is implemented in assembly for precise control.
     * See rockclimb_boot.S for the actual implementation.
     */

    /* Restore SP first */
    __asm__ volatile (
        "mov.w %0, SP\n"
        :
        : "m" (__nvm_sp)
    );

    /* Restore registers R4-R15 */
    __asm__ volatile (
        "mov.w %0, R4\n"
        "mov.w %1, R5\n"
        "mov.w %2, R6\n"
        "mov.w %3, R7\n"
        :
        : "m" (__nvm_regs[0]), "m" (__nvm_regs[1]),
          "m" (__nvm_regs[2]), "m" (__nvm_regs[3])
    );

    __asm__ volatile (
        "mov.w %0, R8\n"
        "mov.w %1, R9\n"
        "mov.w %2, R10\n"
        "mov.w %3, R11\n"
        :
        : "m" (__nvm_regs[4]), "m" (__nvm_regs[5]),
          "m" (__nvm_regs[6]), "m" (__nvm_regs[7])
    );

    __asm__ volatile (
        "mov.w %0, R12\n"
        "mov.w %1, R13\n"
        "mov.w %2, R14\n"
        "mov.w %3, R15\n"
        :
        : "m" (__nvm_regs[8]), "m" (__nvm_regs[9]),
          "m" (__nvm_regs[10]), "m" (__nvm_regs[11])
    );

    /* Jump to saved PC */
    __asm__ volatile (
        "mov.w %0, PC\n"
        :
        : "m" (__nvm_pc)
    );

    /* Should never reach here */
    while(1);
#else
    /* Non-MSP430 stub - just loop forever */
    while(1);
#endif
}

/* ============================================================================
 * Interrupt Service Routines
 * ============================================================================ */

#ifdef __MSP430__
/*
 * Comparator_A interrupt handler.
 * Wakes CPU from LPM when voltage rises above threshold.
 */
#pragma vector=COMPARATORA_VECTOR
__interrupt void comparator_isr(void) {
    /* Clear interrupt flag */
    CACTL1 &= ~CAIFG;

    /* Exit LPM4 on return from interrupt */
    __bic_SR_register_on_exit(LPM4_bits);
}
#endif
