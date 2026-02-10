/*
 * Test: Energy validation - failing case
 *
 * Checkpoint only every K=50 iterations, but capacity=100 and each
 * iteration uses ~5-10 energy units. 50 iterations * ~5 = ~250 energy,
 * which exceeds capacity of 100.
 *
 * Expected: Energy violation detected, program aborts.
 */

#include <stdio.h>

/* Provided by energy_validate_runtime */
void checkpoint(void);

volatile int sink;

int main(void) {
    int sum = 0;

    for (int i = 0; i < 100; i++) {
        sum += i;
        sink = sum;

        /* Checkpoint only every 50 iterations - too infrequent! */
        if (i % 50 == 49) {
            checkpoint();
        }
    }

    printf("FAIL: should not reach here, sum=%d\n", sum);
    return 0;
}
