/*
 * Uninstrumented Boot Routine for MSP430
 *
 * Minimal boot: only disables WDT. GPIO unlock and clock configuration
 * are deferred to timing_gpio_init() in benchmark.h so that P3.4 is
 * driven low immediately after unlock (no floating window that would
 * create a spurious pulse on the Saleae capture).
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

    /* GPIO unlock and clock config are handled by timing_gpio_init()
       in BENCH_INIT(). Doing them here would leave P3.4 floating
       between boot and timing_gpio_init, causing a spurious pulse. */

    /* Fall through to next CRT section */

    .size   _uninstrumented_boot, .-_uninstrumented_boot

    .end
