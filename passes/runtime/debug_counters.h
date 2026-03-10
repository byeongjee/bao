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
 *       DEBUG_INIT();
 *       // ... benchmark code ...
 *       DEBUG_EXIT();
 *       return 0;
 *   }
 *
 * Compile with -DDEBUG_COUNTERS to enable. Without it, macros expand to nothing.
 */

#ifndef DEBUG_COUNTERS_H
#define DEBUG_COUNTERS_H

#ifdef DEBUG_COUNTERS

void debug_init(void);
void debug_exit(void);

#define DEBUG_INIT() debug_init()
#define DEBUG_EXIT() debug_exit()

#else

#define DEBUG_INIT()
#define DEBUG_EXIT()

#endif /* DEBUG_COUNTERS */

#endif /* DEBUG_COUNTERS_H */
