/*
 * Test: Energy validation - passing case
 *
 * Conditional checkpoint every K=5 iterations.
 * With capacity=100 and all instruction costs=1, each iteration costs
 * a small amount (~5-10 instructions), so 5 iterations easily fits.
 *
 * Expected: No energy violation, clean exit.
 */

#include <stdio.h>

/* Provided by energy_validate_runtime */
void checkpoint(void);

volatile int sink;

int main(void) {
    int sum = 0;

    for (int i = 0; i < 20; i++) {
        sum += i;
        sink = sum;

        /* Checkpoint every 5 iterations */
        if (i % 5 == 4) {
            checkpoint();
        }
    }

    printf("PASS: sum=%d\n", sum);
    return 0;
}
