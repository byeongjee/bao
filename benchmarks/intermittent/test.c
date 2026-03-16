/**
 * Minimal test benchmark: single loop summing array elements.
 *
 * Expected result: 1024 * 42 = 43008.
 * Uses a 1024-element array so the compiler cannot unroll the loop.
 * Useful for verifying checkpoint insertion with minimal noise.
 */

#include "benchmark.h"
#include "loop_tripcount.h"
#include <stdint.h>

#define N 1024

static int data[N] __attribute__((used)) = {[0 ... 1023] = 42};

__attribute__((noinline)) int main(void) {
    BENCH_INIT();

    int sum = 0;
    for (int i = 0; i < N; ++i) {
        __loop_tripcount(N);
        sum += data[i];
    }

    BENCH_EXIT(sum);
    return sum;
}
