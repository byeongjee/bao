/*
 * Test: Diamond CFG (If-Else)
 * CFG: entry -> cond -> then_block -> merge -> exit
 *                    -> else_block ---^
 *
 * Tests that BOTH paths respect capacity constraint.
 * Uses volatile to prevent optimization.
 */

volatile int sink;

int test_diamond(volatile int x) {
    volatile int result = 0;

    if (x > 0) {
        // Then block: smaller (~6 instructions)
        result = x + 1;
        result = result + 2;
        sink = result;
    } else {
        // Else block: larger (~12 instructions)
        result = x - 1;
        result = result - 2;
        result = result - 3;
        result = result - 4;
        result = result - 5;
        result = result - 6;
        sink = result;
    }

    // Merge block
    result = result * 2;
    return result;
}
