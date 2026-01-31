/*
 * RockClimb Test: Memory Checkpointing - Stack Variables
 *
 * Tests memory checkpointing for local stack variables (allocas).
 */

#include "rockclimb_debug.h"

volatile int sink;

int test_memory_stack(int x) {
    DEBUG_REGION(0);
    DEBUG_VAR("x", x);

    int local_a = x + 1;
    int local_b = x + 2;

    local_a = local_a * 2;
    local_a = local_a + 10;
    local_a = local_a * 3;
    sink = local_a;

    local_b = local_b * 2;
    local_b = local_b + 20;
    local_b = local_b * 3;
    sink = local_b;

    DEBUG_VAR("local_a", local_a);
    DEBUG_VAR("local_b", local_b);

    int temp = local_a + local_b;
    temp = temp * 2;
    temp = temp + 30;
    sink = temp;

    DEBUG_VAR("temp", temp);
    DEBUG_CHECKPOINT(0);

    DEBUG_REGION(1);
    int result = local_a + local_b;
    result = result + temp;
    sink = result;

    DEBUG_VAR("result", result);
    return result;
}

int main(void) {
    debug_init();

    DEBUG_PRINT("test_memory_stack(10)\n");
    int result = test_memory_stack(10);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("test_memory_stack(5)\n");
    result = test_memory_stack(5);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("Done.\n");
    while (1) { }
    return 0;
}
