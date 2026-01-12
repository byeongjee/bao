/*
 * Test: Linear Sequence
 * CFG: entry -> block_a -> block_b -> block_c -> exit
 *
 * Tests basic energy propagation through a sequence of blocks.
 * Uses volatile to prevent constant folding.
 */

volatile int sink;

int test_linear(int x) {
    volatile int a, b, c, d;

    // Block A: ~8 instructions (stores + adds)
    a = x + 1;
    a = a + 2;
    a = a + 3;
    sink = a;  // Force materialization

    // Block B: ~8 instructions
    b = a + 4;
    b = b + 5;
    b = b + 6;
    sink = b;

    // Block C: ~8 instructions
    c = b + 7;
    c = c + 8;
    c = c + 9;
    sink = c;

    // Block D: ~8 instructions
    d = c + 10;
    d = d + 11;
    d = d + 12;

    return d;
}
