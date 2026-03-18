/* Regression test: nested loop energy propagation (LoopAnalyzer bug).
 * Inner loop consumes more energy than capacity.
 * Outer loop must NOT be marked loopFitsEntirely — it needs a checkpoint. */

void __loop_tripcount(int);

int g_result;

int main(void) {
    int sum = 0;
    /* Outer loop: 1 iteration */
    for (int i = 0; i < 1; i++) {
        __loop_tripcount(1);
        /* Inner loop: 100 iterations — at O0 each iteration is ~15+ IR
         * instructions (1 energy each), totalling ~1500+ energy which far
         * exceeds capacity (500). */
        for (int j = 0; j < 100; j++) {
            __loop_tripcount(100);
            sum += j * 3 + i;
        }
    }
    g_result = sum;
    return 0;
}
