/*
 * RockClimb Test: Memory Checkpointing - Mixed Variables
 *
 * Tests memory checkpointing for both stack variables (allocas)
 * and global variables together. Both should be correctly identified
 * and checkpointed at region boundaries.
 *
 * Compile with -DDEBUG for UART debug output.
 */

#include "rockclimb_debug.h"

volatile int sink;
int g_accum = 0;

int test_memory_mixed(int x, int y) {
    DEBUG_REGION(0);
    DEBUG_VAR("x (input)", x);
    DEBUG_VAR("y (input)", y);

    // Region 0: Entry block with allocas
    int local_x = x;
    int local_y = y;

    // Modify local and global in region 0
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

    // More computation to force boundary
    int temp = local_x + local_y + g_accum;
    temp = temp * 2;
    sink = temp;

    DEBUG_VAR("temp", temp);

    // === BOUNDARY should be inserted here ===
    DEBUG_CHECKPOINT(0);
    // Region 1: Uses both local and global

    DEBUG_REGION(1);
    int result = local_x + local_y;  // Uses locals (checkpointed)
    result = result + g_accum;       // Uses global (checkpointed)
    result = result * 2;
    sink = result;

    DEBUG_VAR("result", result);
    return result;
}

#ifdef DEBUG
int main(void) {
    debug_init();

    DEBUG_PRINT("Starting test_memory_mixed(10, 20)\n");
    int result = test_memory_mixed(10, 20);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("Starting test_memory_mixed(5, 7)\n");
    result = test_memory_mixed(5, 7);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("Test complete.\n");
    while (1) { }
    return 0;
}
#endif

/*
 * Expected behavior:
 * - local_x: Modified in region 0, read in region 1 -> CHECKPOINT
 * - local_y: Modified in region 0, read in region 1 -> CHECKPOINT
 * - g_accum: Modified in region 0, read in region 1 -> CHECKPOINT
 * - NVM slots created for all three
 * - Restore function restores locals to their alloca slots
 *   and global to its original location
 */
