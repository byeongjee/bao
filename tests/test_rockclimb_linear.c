/*
 * RockClimb Test: Linear Sequence
 * CFG: entry -> block_a -> block_b -> block_c -> exit
 *
 * Tests basic region partitioning through a sequence of blocks.
 * With E_safe limit, should create boundaries when accumulated energy exceeds threshold.
 * Uses volatile to prevent constant folding.
 */

volatile int sink;

int test_rockclimb_linear(int x) {
    volatile int a, b, c, d, e, f;

    // Region 1 starts here (entry block)

    // Block A: ~10 instructions
    a = x + 1;
    a = a + 2;
    a = a + 3;
    a = a + 4;
    sink = a;

    // Block B: ~10 instructions
    b = a + 5;
    b = b + 6;
    b = b + 7;
    b = b + 8;
    sink = b;

    // Region 2 might start here depending on E_safe

    // Block C: ~10 instructions
    c = b + 9;
    c = c + 10;
    c = c + 11;
    c = c + 12;
    sink = c;

    // Block D: ~10 instructions
    d = c + 13;
    d = d + 14;
    d = d + 15;
    d = d + 16;
    sink = d;

    // Region 3 might start here

    // Block E: ~10 instructions
    e = d + 17;
    e = e + 18;
    e = e + 19;
    e = e + 20;
    sink = e;

    // Block F: return
    f = e + 21;
    return f;
}
