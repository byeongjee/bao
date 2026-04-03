/*
 * Canonical constant-trip loop whose chosen strip-mining factor divides N.
 * The optimized canonical rewrite should use fixed-size chunks without the
 * per-chunk min/select bound used for the final partial-chunk form.
 */

void __loop_tripcount(int);

volatile int sink;

void test_stripmine_divisible(void) {
    for (int i = 0; i < 12; ++i) {
        __loop_tripcount(12);
        sink += i;
    }
}
