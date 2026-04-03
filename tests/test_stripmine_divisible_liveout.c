/*
 * Divisible canonical loop with a loop-carried live-out. This exercises the
 * exact-division path that rewrites ExitBlock LCSSA PHIs through outer.latch.
 */

void __loop_tripcount(int);

volatile int values[12];
volatile int sink;

void test_stripmine_divisible_liveout(void) {
    int acc = 0;
    for (int i = 0; i < 12; ++i) {
        __loop_tripcount(12);
        acc += values[i];
    }
    sink = acc;
}
