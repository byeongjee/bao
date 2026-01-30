/*
 * RockClimb Test: Memory Checkpointing with Loop
 *
 * Tests memory checkpointing where a loop creates a mandatory boundary.
 * Variables modified before the loop and read inside the loop
 * should be checkpointed.
 */

volatile int sink;
int g_total = 0;

int test_memory_loop(int n) {
    // Region 0: Entry - setup local variables
    int local_sum = 0;
    int local_multiplier = 2;

    // Initialize values
    local_sum = n + 10;
    local_multiplier = local_multiplier * 3;
    sink = local_sum;

    g_total = 100;
    sink = g_total;

    // === MANDATORY BOUNDARY at loop header ===
    // Region 1: Loop body - reads local_sum, local_multiplier, g_total

    for (int i = 0; i < n; i++) {
        // Reads of checkpointed variables
        local_sum = local_sum + local_multiplier;  // Read and modify
        g_total = g_total + local_sum;              // Read and modify
        sink = local_sum;
    }

    // Region may continue after loop
    return g_total;
}

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
