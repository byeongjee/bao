/*
 * Canonical outer loop with a nested canonical inner loop. The outer cleanup
 * clone must also clone the nested inner loop when N % K != 0.
 */

void __loop_tripcount(int);

volatile int sink;

void test_stripmine_nested(void) {
    for (int i = 0; i < 5; ++i) {
        __loop_tripcount(5);
        for (int j = 0; j < 3; ++j) {
            __loop_tripcount(3);
            sink += i + j;
        }
    }
}
