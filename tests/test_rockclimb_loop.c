/*
 * RockClimb Test: Simple Loop
 * CFG: entry -> loop_header -> loop_body -> (back to header) -> exit
 *
 * Tests mandatory loop header boundary insertion.
 * RockClimb requires boundaries at all loop headers regardless of energy.
 */

volatile int sink;

int test_rockclimb_loop(int n) {
    volatile int sum = 0;
    volatile int i;

    // Entry block
    sink = n;

    // Loop header - MUST be a region boundary in RockClimb
    for (i = 0; i < n; i++) {
        // Loop body - energy accumulates here
        sum = sum + i;
        sum = sum * 2;
        sum = sum - 1;
        sink = sum;
    }

    // Exit block
    return sum;
}
