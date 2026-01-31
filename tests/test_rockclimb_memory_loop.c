/*
 * RockClimb Test: Memory Checkpointing with Loop
 *
 * Tests memory checkpointing where a loop creates a mandatory boundary.
 */

#include "rockclimb_debug.h"

volatile int sink;
int g_total = 0;

int test_memory_loop(int n) {
    DEBUG_REGION(0);

    int local_sum = 0;
    int local_multiplier = 2;

    local_sum = n + 10;
    local_multiplier = local_multiplier * 3;
    sink = local_sum;

    DEBUG_VAR("local_sum", local_sum);
    DEBUG_VAR("local_multiplier", local_multiplier);

    g_total = 100;
    sink = g_total;

    DEBUG_VAR("g_total", g_total);
    DEBUG_CHECKPOINT(0);

    for (int i = 0; i < n; i++) {
        DEBUG_REGION(1);
        local_sum = local_sum + local_multiplier;
        g_total = g_total + local_sum;
        sink = local_sum;
        DEBUG_VAR("local_sum", local_sum);
    }

    DEBUG_PRINT("Final g_total = %d\n", g_total);
    return g_total;
}

int main(void) {
    debug_init();

    DEBUG_PRINT("test_memory_loop(3)\n");
    int result = test_memory_loop(3);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("test_memory_loop(5)\n");
    result = test_memory_loop(5);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("Done.\n");
    while (1) { }
    return 0;
}
