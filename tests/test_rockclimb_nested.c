/*
 * RockClimb Test: Nested Loops
 * CFG: entry -> outer_header -> inner_header -> inner_body -> outer_body -> exit
 *
 * Tests nested loop handling - both loop headers must be region boundaries.
 * Inner loop header should be a boundary even if outer loop header is.
 */

volatile int sink;

int test_rockclimb_nested(int m, int n) {
    volatile int sum = 0;
    volatile int i, j;

    // Entry block
    sink = m + n;

    // Outer loop header - region boundary
    for (i = 0; i < m; i++) {
        volatile int outer_temp = i * 2;
        sink = outer_temp;

        // Inner loop header - also a region boundary
        for (j = 0; j < n; j++) {
            // Inner loop body
            sum = sum + i + j;
            sum = sum * 2;
            sink = sum;
        }

        // Outer loop body continuation
        outer_temp = outer_temp + sum;
        sink = outer_temp;
    }

    // Exit block
    return sum;
}
