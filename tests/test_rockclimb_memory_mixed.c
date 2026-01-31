/*
 * RockClimb Test: Memory Checkpointing - Mixed Variables
 *
 * Tests memory checkpointing for both stack and global variables.
 */

#include "rockclimb_debug.h"

volatile int sink;
int g_accum = 0;

int test_memory_mixed(int x, int y) {
    DEBUG_REGION(0);
    DEBUG_VAR("x", x);
    DEBUG_VAR("y", y);

    int local_x = x;
    int local_y = y;

    local_x = local_x + 10;
    local_x = local_x * 2;
    sink = local_x;

    g_accum = local_x + 5;
    g_accum = g_accum * 3;
    sink = g_accum;

    local_y = local_y + g_accum;
    local_y = local_y * 2;
    sink = local_y;

    DEBUG_VAR("local_x", local_x);
    DEBUG_VAR("local_y", local_y);
    DEBUG_VAR("g_accum", g_accum);

    int temp = local_x + local_y + g_accum;
    temp = temp * 2;
    sink = temp;

    DEBUG_VAR("temp", temp);
    DEBUG_CHECKPOINT(0);

    DEBUG_REGION(1);
    int result = local_x + local_y;
    result = result + g_accum;
    result = result * 2;
    sink = result;

    DEBUG_VAR("result", result);
    return result;
}

int main(void) {
    debug_init();

    DEBUG_PRINT("test_memory_mixed(10, 20)\n");
    int result = test_memory_mixed(10, 20);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("test_memory_mixed(5, 7)\n");
    result = test_memory_mixed(5, 7);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("Done.\n");
    while (1) { }
    return 0;
}
