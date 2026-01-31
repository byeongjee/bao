/*
 * RockClimb Test: Memory Checkpointing - Global Variables
 *
 * Tests memory checkpointing for global variables.
 */

#include "rockclimb_debug.h"

volatile int sink;
int global_counter = 0;
int global_state = 0;

void test_memory_global(int x) {
    DEBUG_REGION(0);
    DEBUG_VAR("x", x);

    global_counter = x + 1;
    global_counter = global_counter + 10;
    global_counter = global_counter * 2;
    sink = global_counter;

    global_state = x + 5;
    global_state = global_state * 3;
    global_state = global_state + 20;
    sink = global_state;

    DEBUG_VAR("global_counter", global_counter);
    DEBUG_VAR("global_state", global_state);

    int temp = global_counter + global_state;
    temp = temp * 2;
    temp = temp + 100;
    sink = temp;

    DEBUG_VAR("temp", temp);
    DEBUG_CHECKPOINT(0);

    DEBUG_REGION(1);
    int result = global_counter + global_state;
    result = result * 2;
    sink = result;

    DEBUG_VAR("result", result);
    global_counter = result;
}

int main(void) {
    debug_init();

    DEBUG_PRINT("test_memory_global(10)\n");
    test_memory_global(10);
    DEBUG_PRINT("global_counter = %d\n\n", global_counter);

    DEBUG_PRINT("test_memory_global(5)\n");
    test_memory_global(5);
    DEBUG_PRINT("global_counter = %d\n\n", global_counter);

    DEBUG_PRINT("Done.\n");
    while (1) { }
    return 0;
}
