/*
 * RockClimb Test: Memory Checkpointing - Global Variables
 *
 * Tests memory checkpointing for global variables.
 * Globals modified in one region and read after a boundary should
 * be checkpointed to NVM.
 *
 * Compile with -DDEBUG for UART debug output.
 */

#include "rockclimb_debug.h"

volatile int sink;
int global_counter = 0;
int global_state = 0;

void test_memory_global(int x) {
    DEBUG_REGION(0);
    DEBUG_VAR("x (input)", x);

    // Region 0: Modify globals
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

    // More work to force boundary
    int temp = global_counter + global_state;
    temp = temp * 2;
    temp = temp + 100;
    sink = temp;

    DEBUG_VAR("temp", temp);

    // === BOUNDARY should be inserted here ===
    DEBUG_CHECKPOINT(0);
    // Region 1: Uses globals (they should be checkpointed)

    DEBUG_REGION(1);
    int result = global_counter + global_state;  // Reads of checkpointed globals
    result = result * 2;
    sink = result;

    DEBUG_VAR("result", result);

    global_counter = result;  // Final write
    DEBUG_VAR("global_counter (final)", global_counter);
}

#ifdef DEBUG
int main(void) {
    debug_init();

    DEBUG_PRINT("Starting test_memory_global(10)\n");
    test_memory_global(10);
    DEBUG_PRINT("global_counter = %d\n\n", global_counter);

    DEBUG_PRINT("Starting test_memory_global(5)\n");
    test_memory_global(5);
    DEBUG_PRINT("global_counter = %d\n\n", global_counter);

    DEBUG_PRINT("Test complete.\n");
    while (1) { }
    return 0;
}
#endif

/*
 * Expected behavior:
 * - global_counter: Modified in region 0, read in region 1 -> CHECKPOINT
 * - global_state: Modified in region 0, read in region 1 -> CHECKPOINT
 * - NVM slots: __nvm_b0_global_counter, __nvm_b0_global_state
 * - Restore function restores both globals from NVM
 */
