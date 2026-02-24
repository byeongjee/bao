/*
 * Test: Exit Block Stress Test
 * CFG: entry -> work -> expensive_exit
 *
 * Tests the exit constraint: y[exit] + Cost(exit) <= capacity
 * Exit block has ~12 instructions, so accumulated energy at exit
 * must be low (with capacity=15).
 * Uses volatile to prevent optimization.
 */

int main(void) {
    volatile int x = 10;
    volatile int a;
    volatile int result;

    // Entry block: small amount of work
    a = x + 1;
    a = a + 2;

    // Expensive exit block: ~12 instructions
    // This forces the solver to ensure y[exit] is low enough
    result = a;
    result = result + 1;
    result = result + 2;
    result = result + 3;
    result = result + 4;
    result = result + 5;
    result = result + 6;
    result = result + 7;
    return result;
}
