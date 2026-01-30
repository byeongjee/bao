/*
 * RockClimb Test: Memory Checkpointing - Stack Variables
 *
 * Tests memory checkpointing for local stack variables (allocas).
 * Variables modified in one region and read after a boundary should
 * be checkpointed to NVM.
 *
 * Compile with -DDEBUG for UART debug output.
 */

#include "rockclimb_debug.h"

volatile int sink;

int test_memory_stack(int x) {
    DEBUG_REGION(0);
    DEBUG_VAR("x (input)", x);

    // Region 0: Entry - allocas happen here
    int local_a = x + 1;
    int local_b = x + 2;

    // Add work to create region boundary
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

    // More work to force boundary
    int temp = local_a + local_b;
    temp = temp * 2;
    temp = temp + 30;
    sink = temp;

    DEBUG_VAR("temp", temp);

    // === BOUNDARY should be inserted here ===
    DEBUG_CHECKPOINT(0);
    // Region 1: Uses local_a and local_b (they should be checkpointed)

    DEBUG_REGION(1);
    int result = local_a + local_b;  // Reads of checkpointed variables
    result = result + temp;
    sink = result;

    DEBUG_VAR("result", result);
    return result;
}

#ifdef DEBUG
int main(void) {
    debug_init();

    DEBUG_PRINT("Starting test_memory_stack(10)\n");
    int result = test_memory_stack(10);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("Starting test_memory_stack(5)\n");
    result = test_memory_stack(5);
    DEBUG_PRINT("Result: %d\n\n", result);

    DEBUG_PRINT("Test complete.\n");
    while (1) { }
    return 0;
}
#endif

/*
 * Expected behavior:
 * - local_a: Modified in region 0, read in region 1 -> CHECKPOINT
 * - local_b: Modified in region 0, read in region 1 -> CHECKPOINT
 * - temp: Modified in region 0, read in region 1 -> CHECKPOINT
 * - NVM slots should be created: __nvm_b0_local_a, __nvm_b0_local_b, etc.
 * - Restore function __restore_boundary_0() should be generated
 * - Recovery dispatcher should be inserted at function entry
 */
