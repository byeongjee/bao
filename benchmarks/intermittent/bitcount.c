/*
 * Bit counting benchmark adapted from ulswap-bench/src/bitcount.
 * Single-file form for intermittent checkpoint insertion analysis.
 */

#include <limits.h>
#include <stdint.h>

#include "debug_counters.h"
#include "loop_tripcount.h"

#define FORCE_INLINE static inline __attribute__((always_inline))

#define NUM_FUNCS 8U
#define RNG_SEED 0x0C728394u
#define ITERATIONS 100000U

static uint32_t g_seed = RNG_SEED;
static uint32_t g_totals[NUM_FUNCS] __attribute__((used));

static const uint8_t g_bits[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

FORCE_INLINE void my_srand(uint32_t new_seed) {
    g_seed = new_seed;
}

FORCE_INLINE uint32_t my_rand(void) {
    g_seed = (uint32_t)(1103515245u * g_seed + 12345u);
    return g_seed;
}

FORCE_INLINE uint32_t bit_count(uint32_t x) {
    uint32_t n = 0;

    if (x != 0U) {
        do {
            __loop_tripcount(32);
            n++;
            x = x & (x - 1U);
        } while (x != 0U);
    }

    return n;
}

FORCE_INLINE uint32_t bitcount(uint32_t i) {
    i = ((i & 0xAAAAAAAAL) >> 1) + (i & 0x55555555L);
    i = ((i & 0xCCCCCCCCL) >> 2) + (i & 0x33333333L);
    i = ((i & 0xF0F0F0F0L) >> 4) + (i & 0x0F0F0F0FL);
    i = ((i & 0xFF00FF00L) >> 8) + (i & 0x00FF00FFL);
    i = ((i & 0xFFFF0000L) >> 16) + (i & 0x0000FFFFL);

    return i;
}

FORCE_INLINE uint32_t ntbl_bitcount(uint32_t x) {
    return g_bits[(uint8_t)(x & 0x0000000FUL)] + g_bits[(uint8_t)((x & 0x000000F0UL) >> 4)] +
           g_bits[(uint8_t)((x & 0x00000F00UL) >> 8)] +
           g_bits[(uint8_t)((x & 0x0000F000UL) >> 12)] +
           g_bits[(uint8_t)((x & 0x000F0000UL) >> 16)] +
           g_bits[(uint8_t)((x & 0x00F00000UL) >> 20)] +
           g_bits[(uint8_t)((x & 0x0F000000UL) >> 24)] +
           g_bits[(uint8_t)((x & 0xF0000000UL) >> 28)];
}

FORCE_INLINE uint32_t ntbl_bitcnt(uint32_t x) {
    uint32_t cnt = 0;
    while (x != 0U) {
        __loop_tripcount(8);
        cnt += g_bits[(uint8_t)(x & 0x0000000FUL)];
        x >>= 4;
    }
    return cnt;
}

FORCE_INLINE uint32_t btbl_bitcnt(uint32_t x) {
    uint32_t cnt = 0;
    while (x != 0U) {
        __loop_tripcount(4);
        cnt += g_bits[(uint8_t)x];
        x >>= 8;
    }
    return cnt;
}

FORCE_INLINE uint32_t BW_btbl_bitcount(uint32_t x) {
    union {
        uint8_t ch[4];
        uint32_t y;
    } u;

    u.y = x;

    return g_bits[u.ch[0]] + g_bits[u.ch[1]] + g_bits[u.ch[3]] + g_bits[u.ch[2]];
}

FORCE_INLINE uint32_t AR_btbl_bitcount(uint32_t x) {
    uint8_t *ptr = (uint8_t *)&x;
    uint32_t accu = g_bits[*ptr++];

    accu += g_bits[*ptr++];
    accu += g_bits[*ptr++];
    accu += g_bits[*ptr];

    return accu;
}

FORCE_INLINE uint32_t bit_shifter(uint32_t x) {
    uint32_t i;
    uint32_t n;

    for (i = n = 0; x && (i < (sizeof(uint32_t) * CHAR_BIT)); ++i, x >>= 1) {
        __loop_tripcount(32);
        n += (x & 1U);
    }

    return n;
}

int main(void) {
    DEBUG_INIT();
    uint32_t i;
    uint32_t j;
    uint32_t num;
    uint32_t set_bits;
    volatile uint32_t checksum = 0;

    my_srand(RNG_SEED);

    for (i = 0; i < NUM_FUNCS; i++) {
        __loop_tripcount(NUM_FUNCS);

        set_bits = 0;
        num = my_rand();

        for (j = 0; j < ITERATIONS; j++) {
            __loop_tripcount(ITERATIONS);
            switch (i) {
            case 0:
                set_bits += bit_count(num);
                break;
            case 1:
                set_bits += bitcount(num);
                break;
            case 2:
                set_bits += ntbl_bitcnt(num);
                break;
            case 3:
                set_bits += ntbl_bitcount(num);
                break;
            case 4:
                set_bits += btbl_bitcnt(num);
                break;
            case 5:
                set_bits += BW_btbl_bitcount(num);
                break;
            case 6:
                set_bits += AR_btbl_bitcount(num);
                break;
            default:
                set_bits += bit_shifter(num);
                break;
            }
            num += 13U;
        }

        g_totals[i] = set_bits;
        checksum ^= (set_bits + (i * 0x9E3779B9u));
    }

    DEBUG_EXIT((int)checksum);
    return (int)checksum;
}
