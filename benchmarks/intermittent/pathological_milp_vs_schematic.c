/*
 * Pathological benchmark for MILP vs SCHEMATIC checkpoint placement.
 *
 * The main loop body is intentionally tuned so that tight-energy settings
 * can execute only a couple of iterations per charge. SCHEMATIC then falls
 * into its "checkpoint every iteration" heuristic, while MILP can use loop
 * strip-mining to amortize checkpoint overhead.
 */

#include <stdint.h>

#include "benchmark.h"
#include "loop_tripcount.h"

#define OUTER_ITERS 4096U

#define MIX_STEP(x, mul, add, sh)                                                                  \
    do {                                                                                           \
        (x) = (x) * (mul) + (add);                                                                 \
        (x) ^= ((x) >> (sh));                                                                      \
    } while (0)

static uint32_t g_state __attribute__((used, section(".fram"))) = 0x12345678u;
static uint32_t g_ring[8] __attribute__((used, section(".fram")));

int main(void) {
    BENCH_INIT();
    uint32_t x = g_state ^ 0x9e3779b9u;

    for (uint32_t i = 0; i < OUTER_ITERS; ++i) {
        __loop_tripcount(OUTER_ITERS);

        x ^= (i * 2654435761u) + 0x7f4a7c15u;
        MIX_STEP(x, 1664525u, 1013904223u + i, 7);
        MIX_STEP(x, 22695477u, 1u + (i << 1), 9);
        MIX_STEP(x, 1103515245u, 12345u + (i << 2), 11);
        MIX_STEP(x, 214013u, 2531011u + (i << 3), 13);
        MIX_STEP(x, 134775813u, 1u + (i << 4), 5);
        MIX_STEP(x, 747796405u, 2891336453u + (i << 5), 10);
        MIX_STEP(x, 277803737u, 206666391u + (i << 6), 12);
        MIX_STEP(x, 1597334677u, 3812015801u + (i << 7), 8);
        MIX_STEP(x, 73856093u, 19349663u + (i << 8), 6);
        MIX_STEP(x, 83492791u, 0x9e3779b9u + (i << 1), 14);
        MIX_STEP(x, 2654435761u, 2246822519u + (i << 2), 7);
        MIX_STEP(x, 3266489917u, 668265263u + (i << 3), 11);

        g_ring[i & 7u] = x;
    }

    g_state = x ^ g_ring[0] ^ g_ring[1] ^ g_ring[2] ^ g_ring[3] ^ g_ring[4] ^ g_ring[5] ^
              g_ring[6] ^ g_ring[7];
    BENCH_EXIT((int)(g_state & 0x7fffffffU));
    return (int)(g_state & 0x7fffffffU);
}
