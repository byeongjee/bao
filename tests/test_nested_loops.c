/*
 * Test: Nested Loops
 * CFG: entry -> outer_header -> inner_header -> inner_body -> inner_header
 *                            -> outer_body   -> outer_header
 *            -> exit
 *
 * Tests that optimizer strongly avoids checkpoints in inner loop
 * (highest frequency = outer_freq * inner_freq).
 * Uses volatile to prevent loop optimizations.
 */

int test_nested_loops(volatile int m, volatile int n) {
    volatile int sum = 0;
    volatile int i, j;

    // Outer loop (frequency ~10)
    for (i = 0; i < m; i++) {
        // Outer body work
        sum = sum + i;

        // Inner loop (frequency ~100 = 10 * 10)
        for (j = 0; j < n; j++) {
            // Inner body: should strongly avoid checkpoint here
            sum = sum + j;
            sum = sum + 1;
        }

        // More outer body work
        sum = sum * 2;
    }

    return sum;
}
