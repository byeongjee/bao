/**
 * Test case for loop-bound annotations consumed by checkpoint insertion flows.
 * Compile with: clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone \
 *               -I../passes/include test_analysis.c -o test_analysis.ll
 * Run with: opt -load-pass-plugin=../passes/build/CheckpointPass.so \
 *               -passes=checkpoint-insert,milp-validate \
 *               -checkpoint-algorithm=milp \
 *               -energy-config=simple_config.json \
 *               -milp-config=../benchmarks/sample_milp_config.json \
 *               -S test_analysis.ll -o /dev/null
 */

#include "loop_tripcount.h"

/* Simple loop with annotated trip count */
int sum_array(int *arr, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        __loop_tripcount(100);
        sum += arr[i];
    }
    return sum;
}

/* Nested loops with different trip counts */
int nested_loops(int n, int m) {
    int result = 0;
    for (int i = 0; i < n; i++) {
        __loop_tripcount(10);
        for (int j = 0; j < m; j++) {
            __loop_tripcount(20);
            result += i * j;
        }
    }
    return result;
}

/* Loop without annotation (uses default bound) */
int unannotated_loop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}

/* Mixed: some loops annotated, some not */
int mixed_loops(int n, int m) {
    int result = 0;

    /* Annotated outer loop */
    for (int i = 0; i < n; i++) {
        __loop_tripcount(5);
        result += i;

        /* Unannotated inner loop (uses default) */
        for (int j = 0; j < m; j++) {
            result += j;
        }
    }

    return result;
}

int main() {
    int arr[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    int r1 = sum_array(arr, 10);
    int r2 = nested_loops(5, 3);
    int r3 = unannotated_loop(10);
    int r4 = mixed_loops(5, 3);

    return r1 + r2 + r3 + r4;
}
