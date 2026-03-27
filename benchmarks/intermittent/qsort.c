/*
 * Sorting benchmark adapted from ulswap-bench/src/qsort.
 * Single-file intermittent variant with local always-inline sort helpers.
 */

#include <stdint.h>

#include "benchmark.h"
#include "loop_tripcount.h"
#include "qsort_input.h"

#define FORCE_INLINE static inline __attribute__((always_inline))

static uint32_t g_checksum_sink __attribute__((used, section(".fram"))) = 0;

FORCE_INLINE uint32_t squared_distance(const Vertex *vertex) {
    uint32_t x = (uint32_t)((int32_t)vertex->x * (int32_t)vertex->x);
    uint32_t y = (uint32_t)((int32_t)vertex->y * (int32_t)vertex->y);
    uint32_t z = (uint32_t)((int32_t)vertex->z * (int32_t)vertex->z);

    return x + y + z;
}

FORCE_INLINE void populate_distances(void) {
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE; ++i) {
        __loop_tripcount(ARRAY_SIZE);
        vertices[i].distance = squared_distance(&vertices[i]);
    }
}

FORCE_INLINE void swap_vertices(Vertex *lhs, Vertex *rhs) {
    Vertex tmp = *lhs;
    *lhs = *rhs;
    *rhs = tmp;
}

FORCE_INLINE int32_t partition_vertices(int32_t left, int32_t right) {
    uint32_t pivot_distance = vertices[left + ((right - left) / 2)].distance;

    while (1) {
        __loop_tripcount(ARRAY_SIZE);

        while (vertices[left].distance < pivot_distance) {
            __loop_tripcount(ARRAY_SIZE);
            left++;
        }

        while (vertices[right].distance > pivot_distance) {
            __loop_tripcount(ARRAY_SIZE);
            right--;
        }

        if (left >= right) {
            return right;
        }

        swap_vertices(&vertices[left], &vertices[right]);
        left++;
        right--;
    }
}

FORCE_INLINE void sort_vertices(void) {
    int16_t left_stack[ARRAY_SIZE];
    int16_t right_stack[ARRAY_SIZE];
    int32_t stack_top = 0;

    left_stack[0] = 0;
    right_stack[0] = ARRAY_SIZE - 1;

    while (stack_top >= 0) {
        __loop_tripcount(ARRAY_SIZE);
        int32_t left = left_stack[stack_top];
        int32_t right = right_stack[stack_top];
        stack_top--;

        while (left < right) {
            __loop_tripcount(ARRAY_SIZE);
            int32_t split = partition_vertices(left, right);
            int32_t left_len = split - left + 1;
            int32_t right_len = right - split;

            if (left_len < right_len) {
                if (split + 1 < right) {
                    stack_top++;
                    left_stack[stack_top] = (int16_t)(split + 1);
                    right_stack[stack_top] = (int16_t)right;
                }
                right = split;
            } else {
                if (left < split) {
                    stack_top++;
                    left_stack[stack_top] = (int16_t)left;
                    right_stack[stack_top] = (int16_t)split;
                }
                left = split + 1;
            }
        }
    }
}

FORCE_INLINE uint32_t checksum_vertices(void) {
    uint32_t i;
    uint32_t checksum = 2166136261u;

    for (i = 0; i < ARRAY_SIZE; ++i) {
        __loop_tripcount(ARRAY_SIZE);
        checksum = (checksum * 16777619u) ^ (uint16_t)vertices[i].x;
        checksum = (checksum * 16777619u) ^ (uint16_t)vertices[i].y;
        checksum = (checksum * 16777619u) ^ (uint16_t)vertices[i].z;
        checksum = (checksum * 16777619u) ^ vertices[i].distance;
    }

    return checksum;
}

int main(void) {
    uint32_t checksum;

    BENCH_INIT();

    populate_distances();
    sort_vertices();
    checksum = checksum_vertices();

    g_checksum_sink = checksum;

    BENCH_EXIT((int)(checksum & 0x7FFFFFFFu));
    return (int)(checksum & 0x7FFFFFFFu);
}
