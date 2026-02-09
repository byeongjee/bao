/*
 * Test: Distributed Checkpoint Stores
 * Tests that the MILP enables checkpoint stores at definition sites
 * for SSA values and memory objects (globals).
 *
 * Uses a global variable and several computations to verify that
 * store_enabled[d] variables are set for definitions that need checkpointing.
 */

volatile int global_counter;

int test_distributed_stores(int x) {
    // Several definitions that may need checkpoint stores
    int a = x + 1;
    int b = a * 2;
    int c = b + 3;

    // Write to global (memory def)
    global_counter = c;

    // More computation depending on the global
    int d = c + global_counter;
    int e = d * 2;

    // Another global write
    global_counter = e;

    return e + global_counter;
}
