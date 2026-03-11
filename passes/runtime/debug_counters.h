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
 *       DEBUG_EXIT(result);
 *       return result;
 *   }
 *
 * Compile with -DDEBUG_COUNTERS to enable. Without it, macros expand to nothing.
 */

#ifndef DEBUG_COUNTERS_H
#define DEBUG_COUNTERS_H

/* End-of-output marker — used by all runtimes, detected by read_serial.py */
#define DEBUG_END_MARKER "[END_OUTPUT]"

#ifdef DEBUG_COUNTERS

void debug_init(void);
void debug_exit(int result);

#define DEBUG_INIT() debug_init()
#define DEBUG_EXIT(result) debug_exit((result))

#else

#define DEBUG_INIT()
#define DEBUG_EXIT(result) ((void)(result))

#endif /* DEBUG_COUNTERS */

#endif /* DEBUG_COUNTERS_H */
