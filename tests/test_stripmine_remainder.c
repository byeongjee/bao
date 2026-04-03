/*
 * Canonical constant-trip loop whose chosen strip-mining factor does not
 * divide N. The rewrite should use fixed-size full chunks plus a peeled
 * cleanup remainder loop, with no per-chunk min/select bound.
 */

void __loop_tripcount(int);

volatile int sink;

void test_stripmine_remainder(void) {
    for (int i = 0; i < 13; ++i) {
        __loop_tripcount(13);
        sink += i;
    }
}
