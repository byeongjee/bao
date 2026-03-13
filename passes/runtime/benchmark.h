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
 * Compile with -DDEBUG_COUNTERS to also enable UART output and NVM counters.
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#ifdef __MSP430__
#include <msp430.h>
#endif

/* End-of-output marker — used by all runtimes, detected by read_serial.py */
#define DEBUG_END_MARKER "[END_OUTPUT]"

/* --- GPIO timing (always on, negligible overhead) --- */
#ifdef __MSP430__
static inline void timing_gpio_init(void) {
    P3DIR |= BIT4;
    P3OUT &= ~BIT4;
}
static inline void timing_gpio_start(void) {
    P3OUT |= BIT4;
}
static inline void timing_gpio_stop(void) {
    P3OUT &= ~BIT4;
}
#else
static inline void timing_gpio_init(void) {}
static inline void timing_gpio_start(void) {}
static inline void timing_gpio_stop(void) {}
#endif

/* --- Benchmark entry/exit macros --- */
#ifdef DEBUG_COUNTERS

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

#define BENCH_INIT()                                                                               \
    do {                                                                                           \
        timing_gpio_init();                                                                        \
        timing_gpio_start();                                                                       \
    } while (0)
#define BENCH_EXIT(result)                                                                         \
    do {                                                                                           \
        timing_gpio_stop();                                                                        \
        (void)(result);                                                                            \
    } while (0)

#endif /* DEBUG_COUNTERS */

#endif /* BENCHMARK_H */
