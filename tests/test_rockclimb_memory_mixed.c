/*
 * RockClimb Test: Memory Checkpointing - Mixed Variables
 *
 * Tests memory checkpointing for both stack variables (allocas)
 * and global variables together. Both should be correctly identified
 * and checkpointed at region boundaries.
 */

volatile int sink;
int g_accum = 0;

int test_memory_mixed(int x, int y) {
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

    // More computation to force boundary
    int temp = local_x + local_y + g_accum;
    temp = temp * 2;
    sink = temp;

    // === BOUNDARY should be inserted here ===
    // Region 1: Uses both local and global

    int result = local_x + local_y;  // Uses locals (checkpointed)
    result = result + g_accum;       // Uses global (checkpointed)
    result = result * 2;
    sink = result;

    return result;
}

/*
 * Expected behavior:
 * - local_x: Modified in region 0, read in region 1 -> CHECKPOINT
 * - local_y: Modified in region 0, read in region 1 -> CHECKPOINT
 * - g_accum: Modified in region 0, read in region 1 -> CHECKPOINT
 * - NVM slots created for all three
 * - Restore function restores locals to their alloca slots
 *   and global to its original location
 */
