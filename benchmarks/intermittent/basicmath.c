/*
 * Basic math benchmark adapted from ulswap-bench/src/basicmath.
 * Single-file form for intermittent checkpoint insertion analysis.
 */

#include <math.h>
#include <stdint.h>

#include "debug_counters.h"
#include "loop_tripcount.h"

#define FORCE_INLINE static inline __attribute__((always_inline))

#define PI_CONST 3.14159265358979323846
#define BITSPERNUM (sizeof(uint32_t) * 8U)
#define TOP2BITS(x) (((x) & (3UL << (BITSPERNUM - 2U))) >> (BITSPERNUM - 2U))

typedef struct {
    uint32_t sqrt;
    uint32_t frac;
} int_sqrt_t;

static double g_accum = 0.0;
static int_sqrt_t g_last_sqrt __attribute__((used));

FORCE_INLINE void solve_cubic(double a, double b, double c, double d, uint32_t *solutions,
                              double *x) {
    double a1 = b / a;
    double a2 = c / a;
    double a3 = d / a;
    double Q = (a1 * a1 - 3.0 * a2) / 9.0;
    double R = (2.0 * a1 * a1 * a1 - 9.0 * a1 * a2 + 27.0 * a3) / 54.0;
    double R2_Q3 = R * R - Q * Q * Q;

    if (R2_Q3 <= 0.0) {
        double theta = acos(R / sqrt(Q * Q * Q));
        *solutions = 3;
        x[0] = -2.0 * sqrt(Q) * cos(theta / 3.0) - a1 / 3.0;
        x[1] = -2.0 * sqrt(Q) * cos((theta + 2.0 * PI_CONST) / 3.0) - a1 / 3.0;
        x[2] = -2.0 * sqrt(Q) * cos((theta + 4.0 * PI_CONST) / 3.0) - a1 / 3.0;
    } else {
        *solutions = 1;
        x[0] = pow(sqrt(R2_Q3) + fabs(R), 1.0 / 3.0);
        x[0] += Q / x[0];
        x[0] *= (R < 0.0) ? 1.0 : -1.0;
        x[0] -= a1 / 3.0;
    }
}

FORCE_INLINE void usqrt(uint32_t x, int_sqrt_t *q) {
    uint32_t a = 0;
    uint32_t r = 0;
    uint32_t e = 0;
    uint32_t i;

    for (i = 0; i < BITSPERNUM; i++) {
        __loop_tripcount(BITSPERNUM);
        r = (r << 2U) + TOP2BITS(x);
        x <<= 2U;
        a <<= 1U;
        e = (a << 1U) + 1U;
        if (r >= e) {
            r -= e;
            a++;
        }
    }

    q->sqrt = a;
    q->frac = r;
}

FORCE_INLINE double rad2deg(double rad) {
    return (180.0 * rad / PI_CONST);
}

FORCE_INLINE double deg2rad(double deg) {
    return (PI_CONST * deg / 180.0);
}

int main(void) {
    DEBUG_INIT();
    double a1 = 1.0;
    double b1 = -10.5;
    double c1 = 32.0;
    double d1 = -30.0;

    double a2 = 1.0;
    double b2 = -4.5;
    double c2 = 17.0;
    double d2 = -30.0;

    double a3 = 1.0;
    double b3 = -3.5;
    double c3 = 22.0;
    double d3 = -31.0;

    double a4 = 1.0;
    double b4 = -13.7;
    double c4 = 1.0;
    double d4 = -35.0;

    double x[3] = {0.0, 0.0, 0.0};
    double X;
    uint32_t solutions = 0;
    uint32_t i;
    int_sqrt_t q;
    volatile double accum = 0.0;

    solve_cubic(a1, b1, c1, d1, &solutions, x);
    accum += x[0];
    if (solutions > 1U) {
        accum += x[1] + x[2];
    }

    solve_cubic(a2, b2, c2, d2, &solutions, x);
    accum += x[0];

    solve_cubic(a3, b3, c3, d3, &solutions, x);
    accum += x[0];

    solve_cubic(a4, b4, c4, d4, &solutions, x);
    accum += x[0];

    for (a1 = 1.0; a1 < 7.0; a1 += 1.0) {
        __loop_tripcount(6);
        for (b1 = 8.0; b1 > 0.0; b1 -= 1.0) {
            __loop_tripcount(8);
            for (c1 = 8.0; c1 < 12.0; c1 += 0.5) {
                __loop_tripcount(8);
                for (d1 = -1.0; d1 > -8.0; d1 -= 1.0) {
                    __loop_tripcount(7);
                    solve_cubic(a1, b1, c1, d1, &solutions, x);
                    accum += x[0];
                }
            }
        }
    }

    for (i = 0; i < 1001U; ++i) {
        __loop_tripcount(1001);
        usqrt(i, &q);
        accum += ((double)q.sqrt * 0.0001) + ((double)q.frac * 0.000001);
    }

    for (X = 0.0; X <= 360.0; X += 1.0) {
        __loop_tripcount(361);
        accum += deg2rad(X);
    }

    for (X = 0.0; X <= (2.0 * PI_CONST + 1e-6); X += (PI_CONST / 180.0)) {
        __loop_tripcount(361);
        accum += rad2deg(X);
    }

    g_accum = accum;
    g_last_sqrt = q;

    DEBUG_EXIT();
    return (int)(((uint64_t)(accum * 1000.0)) & 0x7FFFFFFF);
}
