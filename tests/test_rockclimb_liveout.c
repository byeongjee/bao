/*
 * RockClimb Test: Live-Out Registers
 *
 * Tests distributed checkpointing with live-out analysis.
 * Variables defined in one region but used in another should be checkpointed.
 */

volatile int sink;

int test_rockclimb_liveout(int x) {
    volatile int a, b, c, d;

    // Region 1: Define 'a' and 'b'
    // These are live-out if used in later regions
    a = x + 1;
    a = a + 2;
    a = a + 3;
    sink = a;

    b = a + 10;
    b = b + 20;
    sink = b;

    // Adding more computation to potentially trigger region boundary
    volatile int temp1 = a * b;
    volatile int temp2 = temp1 + a;
    volatile int temp3 = temp2 + b;
    sink = temp3;

    // More computation (might be in Region 2)
    c = a + b;           // Uses a and b (live-out from Region 1)
    c = c + temp3;
    c = c + 100;
    sink = c;

    d = b + c;           // Uses b (live-out) and c
    d = d + 200;
    sink = d;

    // Return uses d
    return d;
}
