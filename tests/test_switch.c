/*
 * Test: Switch Statement (Multiple Successors)
 * CFG: entry -> switch -> case0 -> merge -> exit
 *                      -> case1 ---^
 *                      -> case2 ---^
 *                      -> default -^
 *
 * Tests handling of multiple successors from one block.
 * Different cases have different costs.
 * Uses volatile to prevent optimization.
 */

volatile int sink;

int test_switch(volatile int x) {
    volatile int result = 0;

    switch (x) {
        case 0:
            // Case 0: light (~4 instructions)
            result = 1;
            result = result + 1;
            break;

        case 1:
            // Case 1: medium (~8 instructions)
            result = 2;
            result = result + 2;
            result = result + 3;
            result = result + 4;
            break;

        case 2:
            // Case 2: heavy (~12 instructions)
            result = 3;
            result = result + 5;
            result = result + 6;
            result = result + 7;
            result = result + 8;
            result = result + 9;
            break;

        default:
            // Default: medium (~6 instructions)
            result = -1;
            result = result - 1;
            result = result - 2;
            break;
    }

    // Merge
    sink = result;
    result = result * 10;
    return result;
}
