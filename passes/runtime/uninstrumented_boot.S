/*
 * Uninstrumented Boot Routine for MSP430
 *
 * Minimal boot: disable WDT, unlock GPIO, configure clock.
 * No checkpoint recovery logic — this is the uninstrumented baseline.
 *
 * Placed in .crt_0010 (before data/BSS init) to match the boot timing
 * of instrumented variants.
 */

    .file   "uninstrumented_boot.S"

    .section .crt_0010, "ax", @progbits
    .global _uninstrumented_boot
    .type   _uninstrumented_boot, @function

_uninstrumented_boot:
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
    mov.w   #0x0006, &0x0162    /* CSCTL1 = DCOFSEL_6 = 8 MHz */
    mov.b   #0x00, &0x0161
#endif

    /* Fall through to next CRT section */

    .size   _uninstrumented_boot, .-_uninstrumented_boot

    .end
