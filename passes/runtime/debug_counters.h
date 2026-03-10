/*
 * Debug Counters for MSP430 Intermittent Computing Benchmarks
 *
 * Provides UART output and NVM counters for profiling checkpoint runtimes.
 * Shared across all runtimes (RockClimb, SCHEMATIC, MILP).
 *
 * Usage in benchmarks:
 *   #include "debug_counters.h"
 *   int main(void) {
 *       WDTCTL = WDTPW | WDTHOLD;
 *       debug_init();
 *       // ... benchmark code ...
 *       debug_exit();
 *   }
 *
 * Compile with -DDEBUG_COUNTERS to enable. Without it, calls are no-ops.
 */

#ifndef DEBUG_COUNTERS_H
#define DEBUG_COUNTERS_H

#ifdef DEBUG_COUNTERS

void debug_init(void);
void debug_exit(void);

#else

#define debug_init() ((void)0)
#define debug_exit() ((void)0)

#endif /* DEBUG_COUNTERS */

#endif /* DEBUG_COUNTERS_H */
