/*
 * RockClimb Test: Memory Checkpointing with Loop
 *
 * Tests memory checkpointing where a loop creates a mandatory boundary.
 * Variables modified before the loop and read inside the loop
 * should be checkpointed.
 *
 * Compile with -DDEBUG for UART debug output.
 */

#include "rockclimb_debug.h"

volatile int sink;
int g_total = 0;

int test_memory_loop(int n) {
    DEBUG_REGION(0);

    // Region 0: Entry - setup local variables
    int local_sum = 0;
    int local_multiplier = 2;

    // Initialize values
    local_sum = n + 10;
    local_multiplier = local_multiplier * 3;
    sink = local_sum;

    DEBUG_VAR("local_sum", local_sum);
    DEBUG_VAR("local_multiplier", local_multiplier);

    g_total = 100;
    sink = g_total;

    DEBUG_VAR("g_total", g_total);

    // === MANDATORY BOUNDARY at loop header ===
    // Region 1: Loop body - reads local_sum, local_multiplier, g_total
    DEBUG_CHECKPOINT(0);

    for (int i = 0; i < n; i++) {
        DEBUG_REGION(1);
        DEBUG_VAR("i", i);

        // Reads of checkpointed variables
        local_sum = local_sum + local_multiplier;  // Read and modify
        g_total = g_total + local_sum;              // Read and modify
        sink = local_sum;

        DEBUG_VAR("local_sum", local_sum);
        DEBUG_VAR("g_total", g_total);
    }

    // Region may continue after loop
    DEBUG_PRINT("Final g_total = %d\n", g_total);
    return g_total;
}

#ifdef DEBUG
int main(void) {
    debug_init();

    DEBUG_PRINT("Starting test_memory_loop(3)\n");
    int result = test_memory_loop(3);
    DEBUG_PRINT("Result: %d\n", result);

    DEBUG_PRINT("\nStarting test_memory_loop(5)\n");
    result = test_memory_loop(5);
    DEBUG_PRINT("Result: %d\n", result);

    DEBUG_PRINT("\nTest complete.\n");
    while (1) { /* halt */ }
    return 0;
}
#endif

/*
 * Expected behavior:
 * - local_sum: Modified in region 0, read in region 1 (loop) -> CHECKPOINT
 * - local_multiplier: Modified in region 0, read in region 1 (loop) -> CHECKPOINT
 * - g_total: Modified in region 0, read in region 1 (loop) -> CHECKPOINT
 *
 * The loop header creates a mandatory region boundary.
 * All variables read inside the loop but initialized before it
 * should be checkpointed at the boundary.
 */
