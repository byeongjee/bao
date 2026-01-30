/*
 * RockClimb Test: Memory Checkpointing - Global Variables
 *
 * Tests memory checkpointing for global variables.
 * Globals modified in one region and read after a boundary should
 * be checkpointed to NVM.
 */

volatile int sink;
int global_counter = 0;
int global_state = 0;

void test_memory_global(int x) {
    // Region 0: Modify globals
    global_counter = x + 1;
    global_counter = global_counter + 10;
    global_counter = global_counter * 2;
    sink = global_counter;

    global_state = x + 5;
    global_state = global_state * 3;
    global_state = global_state + 20;
    sink = global_state;

    // More work to force boundary
    int temp = global_counter + global_state;
    temp = temp * 2;
    temp = temp + 100;
    sink = temp;

    // === BOUNDARY should be inserted here ===
    // Region 1: Uses globals (they should be checkpointed)

    int result = global_counter + global_state;  // Reads of checkpointed globals
    result = result * 2;
    sink = result;

    global_counter = result;  // Final write
}

/*
 * Expected behavior:
 * - global_counter: Modified in region 0, read in region 1 -> CHECKPOINT
 * - global_state: Modified in region 0, read in region 1 -> CHECKPOINT
 * - NVM slots: __nvm_b0_global_counter, __nvm_b0_global_state
 * - Restore function restores both globals from NVM
 */
