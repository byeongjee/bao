/*
 * Test: Simple Loop
 * CFG: entry -> loop_header -> loop_body -> loop_header (back edge)
 *                           -> exit
 *
 * Tests frequency weighting - optimizer should prefer checkpoints
 * outside the hot loop body to minimize runtime overhead.
 * Uses volatile to prevent loop optimizations.
 */

int test_simple_loop(volatile int n) {
    volatile int sum = 0;
    volatile int i;

    // Loop: header checks condition, body does work
    for (i = 0; i < n; i++) {
        // Loop body: ~10 instructions (hot - executed n times)
        sum = sum + i;
        sum = sum + 1;
        sum = sum + 2;
        sum = sum + 3;
    }

    return sum;
}
