/*
 * wait_until_vcc_full — Block until the supply capacitor reaches 3.3 V.
 *
 * Used by the HALT_WAIT halt mode for real intermittent-power evaluation:
 * called at every region boundary (after the PC/SP checkpoint), at
 * recovery boot (the board wakes as soon as VCC crosses the ~1.8 V
 * brownout level, well below a full capacitor), and at fresh boot.
 *
 * VCC is measured with the ADC12_B battery monitor: ADC12BATMAP routes
 * AVCC/2 to input channel 31, converted against the internal 2.0 V
 * reference. Between samples the CPU sleeps in LPM3, woken every ~10 ms
 * by Timer_A0 clocked from VLO (~9.4 kHz), so waiting draws ~1 uA and
 * does not fight the capacitor's charging current.
 *
 * Called from the .crt_0010 boot path BEFORE data/BSS initialization,
 * so this file must not use any global variables.
 */

#include <msp430.h>
#include <stdint.h>

#ifdef HALT_WAIT

/* Full capacitor = 3.3 V supply. The battery monitor sees AVCC/2 = 1.65 V;
   against the 2.0 V reference: 4095 * 1.65 / 2.0 = 3378 counts. */
#define VCC_FULL_ADC_COUNTS 3378

/* Sample period in VLO (~9.4 kHz) ticks: ~10 ms. */
#define SAMPLE_PERIOD_TICKS 94

static uint16_t sample_avcc_half(void) {
    ADC12CTL0 |= ADC12ENC | ADC12SC;
    while (ADC12CTL1 & ADC12BUSY)
        ;
    ADC12CTL0 &= ~ADC12ENC;
    return ADC12MEM0;
}

void wait_until_vcc_full(void) {
    /* ACLK <- VLO so Timer_A keeps running in LPM3 (no crystal needed).
       SELS/SELM stay on DCO, matching timing_gpio_init in benchmark.h. */
    CSCTL0_H = CSKEY_H;
    CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL0_H = 0;

    /* Internal 2.0 V reference. Below ~2.2 V supply the reference is out
       of spec, but the resulting samples are far below the threshold and
       VCC only rises while waiting, so early inaccuracy is harmless. */
    while (REFCTL0 & REFGENBUSY)
        ;
    REFCTL0 = REFVSEL_1 | REFON;

    /* ADC12_B: battery monitor (AVCC/2) on channel 31 against VREF.
       Long sample time (256 clocks) for the high-impedance internal
       divider. */
    ADC12CTL0 = ADC12SHT0_8 | ADC12ON;
    ADC12CTL1 = ADC12SHP;
    ADC12CTL2 = ADC12RES_2;
    ADC12CTL3 = ADC12BATMAP;
    ADC12MCTL0 = ADC12INCH_31 | ADC12VRSEL_1;

    while (!(REFCTL0 & REFGENRDY))
        ;

    if (sample_avcc_half() < VCC_FULL_ADC_COUNTS) {
        TA0CCR0 = SAMPLE_PERIOD_TICKS;
        TA0CCTL0 = CCIE;
        TA0CTL = TASSEL__ACLK | MC__UP | TACLR;

        do {
            __bis_SR_register(LPM3_bits | GIE);
        } while (sample_avcc_half() < VCC_FULL_ADC_COUNTS);

        __disable_interrupt();
        TA0CTL = MC__STOP;
        TA0CCTL0 = 0;
        TA0CTL |= TACLR;
    }

    ADC12CTL0 &= ~ADC12ON;
    REFCTL0 &= ~REFON;
}

__attribute__((interrupt(TIMER0_A0_VECTOR))) void __vcc_wait_timer_isr(void) {
    __bic_SR_register_on_exit(LPM3_bits);
}

#endif /* HALT_WAIT */
