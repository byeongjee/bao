/*
 * Benchmark instrumentation for MSP430 intermittent computing.
 *
 * Provides GPIO timing (always on) and optional debug counter macros.
 * Replaces the former debug_counters.h.
 *
 * Usage in benchmarks:
 *   #include "benchmark.h"
 *   int main(void) {
 *       WDTCTL = WDTPW | WDTHOLD;
 *       BENCH_INIT();
 *       // ... benchmark code ...
 *       BENCH_EXIT(result);
 *       return result;
 *   }
 *
 * GPIO timing (P3.4 high/low) is always active.
 * Compile with -DDEVICE_DEBUG to also enable UART output and NVM counters.
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#ifdef __MSP430__
#include <msp430.h>
#endif

/* End-of-output marker — used by all runtimes, detected by read_serial.py */
#define DEBUG_END_MARKER "[END_OUTPUT]"

/* --- GPIO timing (pulse-based, BOR-safe) ---
 *
 * Start and stop each emit a positive pulse on P3.4.  GPIO is LOW
 * during execution, so BOR resets (which reset port registers to LOW)
 * are invisible to the Saleae capture.
 *
 * The Saleae triggers on PULSE_HIGH with min_pulse_width = 1 ms.
 * Start pulse is ~10 us (below threshold — ignored by trigger).
 * Stop pulse is ~5 ms   (above threshold — fires the trigger).
 *
 * Execution time is measured in post-processing as:
 *   falling edge of the unique short start pulse before the stop pulse →
 *   rising edge of the first long stop pulse.
 */
#ifdef __MSP430__
#ifndef F_CPU
#define F_CPU 1000000UL
#endif

/* Portable volatile delay — works with both clang and msp430-elf-gcc.
 * ~4 cycles per iteration on MSP430.
 * noinline: must not be inlined into main() where RockClimb would
 * instrument the delay loop with region boundaries. */
__attribute__((noinline)) static void _timing_delay_cycles(unsigned long cycles) {
    volatile unsigned long n = cycles / 4;
    while (n--)
        __asm__ volatile("");
}

__attribute__((noinline, used)) static void timing_gpio_init(void) {
    PM5CTL0 &= ~LOCKLPM5;

    /* FRAM wait states: 1 wait state required above 8 MHz */
#if F_CPU > 8000000UL
    FRCTL0 = FRCTLPW | NWAITS_1;
#endif

    /* Configure DCO to F_CPU */
    CSCTL0_H = CSKEY_H;
#if F_CPU == 16000000UL
    CSCTL1 = DCORSEL | DCOFSEL_4; /* 16 MHz */
#elif F_CPU == 8000000UL
    CSCTL1 = DCOFSEL_6; /* 8 MHz */
#elif F_CPU == 1000000UL
    CSCTL1 = DCOFSEL_0; /* 1 MHz */
#else
#error "Unsupported F_CPU — use 1000000, 8000000, or 16000000"
#endif
    CSCTL2 = SELA__VLOCLK | SELS__DCOCLK | SELM__DCOCLK;
    CSCTL3 = DIVA__1 | DIVS__1 | DIVM__1;
    CSCTL0_H = 0;

    P3DIR |= BIT4;
    P3OUT &= ~BIT4;
}
__attribute__((noinline, used)) static void timing_gpio_start(void) {
    P3DIR |= BIT4; /* Ensure output mode (BOR resets P3DIR) */
    P3OUT |= BIT4;
    _timing_delay_cycles(F_CPU / 100000UL); /* 10 us */
    P3OUT &= ~BIT4;
}
__attribute__((noinline, used)) static void timing_gpio_stop(void) {
    /* Reconfigure clock + GPIO — BOR resets both DCO and port registers */
    timing_gpio_init();
    P3OUT |= BIT4;
    _timing_delay_cycles(F_CPU * 5UL / 1000UL); /* 5 ms */
    P3OUT &= ~BIT4;
}
#else
static inline void timing_gpio_init(void) {}
static inline void timing_gpio_start(void) {}
static inline void timing_gpio_stop(void) {}
#endif

/* --- Benchmark entry/exit macros --- */
#ifdef DEVICE_DEBUG

void debug_init(void);
void debug_exit(int result);

#define BENCH_INIT()                                                                               \
    do {                                                                                           \
        debug_init();                                                                              \
        timing_gpio_init();                                                                        \
        timing_gpio_start();                                                                       \
    } while (0)
#define BENCH_EXIT(result)                                                                         \
    do {                                                                                           \
        timing_gpio_stop();                                                                        \
        debug_exit((result));                                                                      \
    } while (0)

#else

void bench_halt(void);

#define BENCH_INIT()                                                                               \
    do {                                                                                           \
        timing_gpio_init();                                                                        \
        timing_gpio_start();                                                                       \
    } while (0)
#define BENCH_EXIT(result)                                                                         \
    do {                                                                                           \
        timing_gpio_stop();                                                                        \
        (void)(result);                                                                            \
        bench_halt();                                                                              \
    } while (0)

#endif /* DEVICE_DEBUG */

#endif /* BENCHMARK_H */
