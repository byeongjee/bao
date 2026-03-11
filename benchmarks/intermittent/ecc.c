/*
 * Elliptic Curve Diffie-Hellman (ECDH) benchmark for intermittent computing.
 *
 * Uses NIST B-233 binary field curve (Koblitz curve).
 * Preprocessed single-file version for MILP checkpoint insertion analysis.
 *
 * Source: ulswap-bench/src/ecc/
 */

#include "debug_counters.h"
#include "loop_tripcount.h"
#include <stdint.h>

#define FORCE_INLINE static inline __attribute__((always_inline))

/* --- Curve parameters for NIST B-233 --- */

#define CURVE_DEGREE 233
#define ECC_PRV_KEY_SIZE 32
#define ECC_PUB_KEY_SIZE 64

#define BITVEC_MARGIN 3
#define BITVEC_NBITS (CURVE_DEGREE + BITVEC_MARGIN)
#define BITVEC_NWORDS ((BITVEC_NBITS + 31) / 32)         /* 8 */
#define BITVEC_NBYTES (sizeof(uint32_t) * BITVEC_NWORDS) /* 32 */

#define CONST_TIME 0
#define ECDH_COFACTOR_VARIANT 0

#define coeff_a 1
#define cofactor 2

/* --- Types --- */

typedef uint32_t bitvec_t[BITVEC_NWORDS];
typedef bitvec_t gf2elem_t;
typedef bitvec_t scalar_t;

/* --- Curve constants (NIST B-233, no annotation) --- */

static const gf2elem_t polynomial = {0x00000001, 0x00000000, 0x00000400, 0x00000000,
                                     0x00000000, 0x00000000, 0x00000000, 0x00000200};
static const gf2elem_t coeff_b = {0x7d8f90ad, 0x81fe115f, 0x20e9ce42, 0x213b333b,
                                  0x0923bb58, 0x332c7f8c, 0x647ede6c, 0x00000066};
/* --- Key data for NIST B-233 (const, no annotation) --- */

static const uint8_t prv_a[ECC_PRV_KEY_SIZE] = {
    0x2F, 0x46, 0x53, 0xB8, 0x26, 0x67, 0x5E, 0x49, 0x72, 0x6B, 0x53, 0xE4, 0x99, 0x55, 0x09, 0x99,
    0xBE, 0x39, 0x47, 0x60, 0xF2, 0x91, 0x87, 0x1F, 0x8F, 0x69, 0x8F, 0x5A, 0x1D, 0x00, 0x00, 0x00};
static const uint8_t prv_b[ECC_PRV_KEY_SIZE] = {
    0x3E, 0xC0, 0x0C, 0xAF, 0x89, 0x04, 0x52, 0x39, 0x97, 0x7D, 0x51, 0x4F, 0xDA, 0x58, 0xAE, 0x16,
    0xA9, 0x63, 0x74, 0x67, 0x94, 0x93, 0xB1, 0xF7, 0x66, 0x7B, 0x29, 0x1C, 0xCE, 0x00, 0x00, 0x00};
static const uint8_t pub_a[ECC_PUB_KEY_SIZE] = {
    0xAF, 0x08, 0xF4, 0x95, 0x0A, 0x27, 0x5F, 0x50, 0x96, 0x38, 0x69, 0x45, 0x9D, 0x40, 0xDF, 0x2B,
    0x25, 0xA9, 0x63, 0x35, 0xD4, 0xAC, 0x61, 0x69, 0x2D, 0x3D, 0x1F, 0xE2, 0x38, 0x00, 0x00, 0x00,
    0x31, 0xAE, 0x94, 0xD6, 0x9C, 0x7D, 0xA4, 0x44, 0x67, 0xC7, 0x1E, 0x66, 0xD6, 0x36, 0x92, 0x6F,
    0x8F, 0x42, 0xB9, 0xBF, 0x71, 0xB4, 0x4F, 0x5B, 0x41, 0xAD, 0xAA, 0x86, 0x82, 0x01, 0x00, 0x00};
static const uint8_t pub_b[ECC_PUB_KEY_SIZE] = {
    0x70, 0xF7, 0x8E, 0xB9, 0x4E, 0x59, 0x98, 0xF8, 0xB3, 0x3A, 0x48, 0xE2, 0x3C, 0x12, 0x8A, 0x9C,
    0x06, 0xE9, 0x34, 0xD6, 0xBC, 0x32, 0xAD, 0xDC, 0xFD, 0x2D, 0x03, 0xA3, 0xC7, 0x00, 0x00, 0x00,
    0x77, 0xAF, 0xA1, 0xA4, 0x31, 0x68, 0xBA, 0xF6, 0x78, 0x29, 0xCB, 0xA6, 0xDC, 0xA3, 0x52, 0x63,
    0xC0, 0xAB, 0x59, 0x53, 0x0C, 0x74, 0xEC, 0x46, 0x3E, 0x20, 0xDF, 0x38, 0x62, 0x00, 0x00, 0x00};

/* --- Mutable globals --- */

__attribute__((used)) static uint8_t sec_a[ECC_PUB_KEY_SIZE];

__attribute__((used)) static uint8_t sec_b[ECC_PUB_KEY_SIZE];

/* ========================================================================= */
/*  Bit-vector operations                                                    */
/* ========================================================================= */

FORCE_INLINE int bitvec_get_bit(const bitvec_t x, const uint32_t idx) {
    return ((x[idx / 32U] >> (idx & 31U)) & 1U);
}

FORCE_INLINE void bitvec_copy(bitvec_t x, const bitvec_t y) {
    int i;
    for (i = 0; i < BITVEC_NWORDS; ++i) {
        __loop_tripcount(8);
        x[i] = y[i];
    }
}

FORCE_INLINE void bitvec_swap(bitvec_t x, bitvec_t y) {
    bitvec_t tmp;
    bitvec_copy(tmp, x);
    bitvec_copy(x, y);
    bitvec_copy(y, tmp);
}

/* fast version (CONST_TIME=0) */
FORCE_INLINE int bitvec_equal(const bitvec_t x, const bitvec_t y) {
    int i;
    for (i = 0; i < BITVEC_NWORDS; ++i) {
        __loop_tripcount(8);
        if (x[i] != y[i]) {
            return 0;
        }
    }
    return 1;
}

FORCE_INLINE void bitvec_set_zero(bitvec_t x) {
    int i;
    for (i = 0; i < BITVEC_NWORDS; ++i) {
        __loop_tripcount(8);
        x[i] = 0;
    }
}

/* fast version (CONST_TIME=0) */
FORCE_INLINE int bitvec_is_zero(const bitvec_t x) {
    uint32_t i = 0;
    while (i < BITVEC_NWORDS) {
        __loop_tripcount(8);
        if (x[i] != 0) {
            break;
        }
        i += 1;
    }
    return (i == BITVEC_NWORDS);
}

/* return the number of the highest one-bit + 1 */
FORCE_INLINE int bitvec_degree(const bitvec_t x) {
    int i = BITVEC_NWORDS * 32;

    /* Start at the back of the vector (MSB) */
    const uint32_t *p = x + BITVEC_NWORDS;

    /* Skip empty / zero words */
    while ((i > 0) && (*(--p)) == 0) {
        __loop_tripcount(8);
        i -= 32;
    }
    /* Run through rest if count is not multiple of bitsize of DTYPE */
    if (i != 0) {
        uint32_t u32mask = ((uint32_t)1 << 31);
        while (((*p) & u32mask) == 0) {
            __loop_tripcount(32);
            u32mask >>= 1;
            i -= 1;
        }
    }
    return i;
}

/* left-shift by 'nbits' digits */
FORCE_INLINE void bitvec_lshift(bitvec_t x, const bitvec_t y, int nbits) {
    int nwords = (nbits / 32);

    int i, j;
    for (i = 0; i < nwords; ++i) {
        __loop_tripcount(8);
        x[i] = 0;
    }
    j = 0;
    while (i < BITVEC_NWORDS) {
        __loop_tripcount(8);
        x[i] = y[j];
        i += 1;
        j += 1;
    }

    /* Shift the rest if count was not multiple of bitsize of DTYPE */
    nbits &= 31;
    if (nbits != 0) {
        int k;
        for (k = (BITVEC_NWORDS - 1); k > 0; --k) {
            __loop_tripcount(8);
            x[k] = (x[k] << nbits) | (x[k - 1] >> (32 - nbits));
        }
        x[0] <<= nbits;
    }
}

/* ========================================================================= */
/*  GF(2^m) arithmetic                                                       */
/* ========================================================================= */

FORCE_INLINE void gf2field_set_one(gf2elem_t x) {
    x[0] = 1;
    int i;
    for (i = 1; i < BITVEC_NWORDS; ++i) {
        __loop_tripcount(8);
        x[i] = 0;
    }
}

/* fast version (CONST_TIME=0) */
FORCE_INLINE int gf2field_is_one(const gf2elem_t x) {
    if (x[0] != 1) {
        return 0;
    }
    int i;
    for (i = 1; i < BITVEC_NWORDS; ++i) {
        __loop_tripcount(8);
        if (x[i] != 0) {
            break;
        }
    }
    return (i == BITVEC_NWORDS);
}

/* galois field(2^m) addition is modulo 2, so XOR is used - 'z := a + b' */
FORCE_INLINE void gf2field_add(gf2elem_t z, const gf2elem_t x, const gf2elem_t y) {
    int i;
    for (i = 0; i < BITVEC_NWORDS; ++i) {
        __loop_tripcount(8);
        z[i] = (x[i] ^ y[i]);
    }
}

/* increment element */
FORCE_INLINE void gf2field_inc(gf2elem_t x) {
    x[0] ^= 1;
}

/* field multiplication 'z := (x * y)' */
FORCE_INLINE void gf2field_mul(gf2elem_t z, const gf2elem_t x, const gf2elem_t y) {
    int i;
    gf2elem_t tmp;

    bitvec_copy(tmp, x);

    /* LSB set? Then start with x */
    if (bitvec_get_bit(y, 0) != 0) {
        bitvec_copy(z, x);
    } else {
        bitvec_set_zero(z);
    }

    /* Then add 2^i * x for the rest */
    for (i = 1; i < CURVE_DEGREE; ++i) {
        __loop_tripcount(233);

        /* lshift 1 - doubling the value of tmp */
        bitvec_lshift(tmp, tmp, 1);

        /* Modulo reduction polynomial if degree(tmp) > CURVE_DEGREE */
        if (bitvec_get_bit(tmp, CURVE_DEGREE)) {
            gf2field_add(tmp, tmp, polynomial);
        }

        /* Add 2^i * tmp if this factor in y is non-zero */
        if (bitvec_get_bit(y, i)) {
            gf2field_add(z, z, tmp);
        }
    }
}

/* field inversion 'z := 1/x' */
FORCE_INLINE void gf2field_inv(gf2elem_t z, const gf2elem_t x) {
    gf2elem_t u, v, g, h;
    int i;

    bitvec_copy(u, x);
    bitvec_copy(v, polynomial);
    bitvec_set_zero(g);
    gf2field_set_one(z);

    while (!gf2field_is_one(u)) {
        __loop_tripcount(466);

        i = (bitvec_degree(u) - bitvec_degree(v));

        if (i < 0) {
            bitvec_swap(u, v);
            bitvec_swap(g, z);
            i = -i;
        }
        bitvec_lshift(h, v, i);
        gf2field_add(u, u, h);
        bitvec_lshift(h, g, i);
        gf2field_add(z, z, h);
    }
}

/* ========================================================================= */
/*  Elliptic curve point operations                                          */
/* ========================================================================= */

FORCE_INLINE void gf2point_copy(gf2elem_t x1, gf2elem_t y1, const gf2elem_t x2,
                                const gf2elem_t y2) {
    bitvec_copy(x1, x2);
    bitvec_copy(y1, y2);
}

FORCE_INLINE void gf2point_set_zero(gf2elem_t x, gf2elem_t y) {
    bitvec_set_zero(x);
    bitvec_set_zero(y);
}

FORCE_INLINE int gf2point_is_zero(const gf2elem_t x, const gf2elem_t y) {
    return (bitvec_is_zero(x) && bitvec_is_zero(y));
}

/* double the point (x,y) */
FORCE_INLINE void gf2point_double(gf2elem_t x, gf2elem_t y) {
    /* iff P = O (zero or infinity): 2 * P = P */
    if (bitvec_is_zero(x)) {
        bitvec_set_zero(y);
    } else {
        gf2elem_t l;

        gf2field_inv(l, x);
        gf2field_mul(l, l, y);
        gf2field_add(l, l, x);
        gf2field_mul(y, x, x);
        gf2field_mul(x, l, l);
        /* coeff_a == 1 */
        gf2field_inc(l);
        gf2field_add(x, x, l);
        gf2field_mul(l, l, x);
        gf2field_add(y, y, l);
    }
}

/* add two points together (x1, y1) := (x1, y1) + (x2, y2) */
FORCE_INLINE void gf2point_add(gf2elem_t x1, gf2elem_t y1, const gf2elem_t x2, const gf2elem_t y2) {
    if (!gf2point_is_zero(x2, y2)) {
        if (gf2point_is_zero(x1, y1)) {
            gf2point_copy(x1, y1, x2, y2);
        } else {
            if (bitvec_equal(x1, x2)) {
                if (bitvec_equal(y1, y2)) {
                    gf2point_double(x1, y1);
                } else {
                    gf2point_set_zero(x1, y1);
                }
            } else {
                gf2elem_t a, b, c, d;

                gf2field_add(a, y1, y2);
                gf2field_add(b, x1, x2);
                gf2field_inv(c, b);
                gf2field_mul(c, c, a);
                gf2field_mul(d, c, c);
                gf2field_add(d, d, c);
                gf2field_add(d, d, b);
                /* coeff_a == 1 */
                gf2field_inc(d);
                gf2field_add(x1, x1, d);
                gf2field_mul(a, x1, c);
                gf2field_add(a, a, d);
                gf2field_add(y1, y1, a);
                bitvec_copy(x1, d);
            }
        }
    }
}

/* point multiplication via double-and-add algorithm (CONST_TIME=0) */
FORCE_INLINE void gf2point_mul(gf2elem_t x, gf2elem_t y, const scalar_t exp) {
    gf2elem_t tmpx, tmpy;
    int i;
    int nbits = bitvec_degree(exp);

    gf2point_set_zero(tmpx, tmpy);

    for (i = (nbits - 1); i >= 0; --i) {
        __loop_tripcount(233);
        gf2point_double(tmpx, tmpy);
        if (bitvec_get_bit(exp, i)) {
            gf2point_add(tmpx, tmpy, x, y);
        }
    }
    gf2point_copy(x, y, tmpx, tmpy);
}

/* check if y^2 + x*y = x^3 + a*x^2 + coeff_b holds */
FORCE_INLINE int gf2point_on_curve(const gf2elem_t x, const gf2elem_t y) {
    gf2elem_t a, b;

    if (gf2point_is_zero(x, y)) {
        return 1;
    } else {
        gf2field_mul(a, x, x);
        /* coeff_a == 1 */
        gf2field_mul(b, a, x);
        gf2field_add(a, a, b);
        gf2field_add(a, a, coeff_b);
        gf2field_mul(b, y, y);
        gf2field_add(a, a, b);
        gf2field_mul(b, x, y);

        return bitvec_equal(a, b);
    }
}

/* ========================================================================= */
/*  ECDH shared secret                                                       */
/* ========================================================================= */

FORCE_INLINE int ecdh_shared_secret(const uint8_t *private_key, const uint8_t *others_pub,
                                    uint8_t *output) {
    /* Basic validation of other party's public key */
    if (!gf2point_is_zero((uint32_t *)others_pub, (uint32_t *)(others_pub + BITVEC_NBYTES)) &&
        gf2point_on_curve((uint32_t *)others_pub, (uint32_t *)(others_pub + BITVEC_NBYTES))) {
        /* Copy other side's public key to output */
        unsigned int i;
        for (i = 0; i < (BITVEC_NBYTES * 2); ++i) {
            __loop_tripcount(64);
            output[i] = others_pub[i];
        }

        /* Multiply other side's public key with own private key */
        gf2point_mul((uint32_t *)output, (uint32_t *)(output + BITVEC_NBYTES),
                     (const uint32_t *)private_key);

        return 1;
    } else {
        return 0;
    }
}

/* ========================================================================= */
/*  Main                                                                     */
/* ========================================================================= */

__attribute__((noinline)) int main(void) {
    DEBUG_INIT();
    ecdh_shared_secret(prv_a, pub_b, sec_a);
    ecdh_shared_secret(prv_b, pub_a, sec_b);

    /* Simple comparison */
    int match = 1;
    int i;
    for (i = 0; i < ECC_PUB_KEY_SIZE; i++) {
        __loop_tripcount(64);
        if (sec_a[i] != sec_b[i])
            match = 0;
    }
    DEBUG_EXIT(match);
    return match;
}
