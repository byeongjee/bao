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
        vertices[i].distance = squared_distance(&vertices[i]);
    }
}

FORCE_INLINE void sort_vertices(void) {
    uint32_t i;

    for (i = 1; i < ARRAY_SIZE; ++i) {
        Vertex key = vertices[i];
        int32_t j = (int32_t)i - 1;

        while ((j >= 0) && (vertices[j].distance > key.distance)) {
            vertices[j + 1] = vertices[j];
            j--;
        }

        vertices[j + 1] = key;
    }
}

FORCE_INLINE uint32_t checksum_vertices(void) {
    uint32_t i;
    uint32_t checksum = 2166136261u;

    for (i = 0; i < ARRAY_SIZE; ++i) {
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
