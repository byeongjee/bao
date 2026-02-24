/*
 * Test: Loop Chunking (fallback for non-strip-mineable loops)
 *
 * These loops have data-dependent trip counts and/or non-canonical
 * induction variables, so they cannot be strip-mined.  The chunking
 * fallback should wrap each loop in an outer/inner structure with a
 * counter-bounded inner loop of at most K iterations.
 *
 * After chunking the output IR should contain outer.header and
 * counter.check blocks, and the summary should report chunked loops > 0.
 */

/* Data-dependent while loop: trip count depends on runtime value */
int test_while_shift(int e) {
    int count = 0;
    while (e > 0) {
        e >>= 1;
        count++;
    }
    return count;
}

/* Countdown from a runtime value */
int test_countdown(int n) {
    int sum = 0;
    while (n > 0) {
        sum += n;
        n--;
    }
    return sum;
}

/* Non-linear stepping: value halves each iteration */
int test_nonlinear_step(int x) {
    int steps = 0;
    while (x >= 10) {
        x = x / 3;
        steps++;
    }
    return steps;
}

/* Loop with accumulator and data-dependent exit */
int test_accumulator(int *arr, int len) {
    int sum = 0;
    int i = 0;
    while (i < len) {
        sum += arr[i];
        i++;
    }
    return sum;
}

int main(void) {
    int arr[] = {1, 2, 3, 4, 5};
    int r = 0;
    r += test_while_shift(255);
    r += test_countdown(10);
    r += test_nonlinear_step(1000);
    r += test_accumulator(arr, 5);
    return r == 0;
}
