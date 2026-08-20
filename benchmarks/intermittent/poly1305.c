/*
 * Poly1305 MAC - Preprocessed benchmark for intermittent computing.
 * Based on OpenSSL poly1305 implementation (Apache 2.0).
 * Simplified: concrete struct instead of opaque double[], no function pointers,
 * uint32_t instead of size_t for MSP430.
 */
#include "benchmark.h"
#include "loop_tripcount.h"
#include <stdint.h>

#define FORCE_INLINE static inline __attribute__((always_inline))

#define POLY1305_BLOCK_SIZE 16

/* --- Types --- */

typedef struct {
    uint32_t h[5];
    uint32_t r[4];
} poly1305_internal;

typedef struct {
    poly1305_internal st;
    uint32_t nonce[4];
    uint8_t data[16];
    uint32_t num;
} poly1305_ctx_t;

/* --- Mutable globals --- */

poly1305_ctx_t g_ctx;
uint8_t g_hash[16] __attribute__((section(".fram")));
uint8_t g_msg[128];

/* --- Const data (no annotation) --- */

/* Key from RFC 7539 */
static const uint8_t key[32] = {0x85, 0xd6, 0xbe, 0x78, 0x57, 0x55, 0x6d, 0x33, 0x7f, 0x44, 0x52,
                                0xfe, 0x42, 0xd5, 0x06, 0xa8, 0x01, 0x03, 0x80, 0x8a, 0xfb, 0x0d,
                                0xb2, 0xfd, 0x4a, 0xbf, 0xf6, 0xaf, 0x41, 0x49, 0xf5, 0x1b};

/* 128-byte test input */
#define TEST_DATA_LEN 128
#define NUM_MESSAGES 256
static const uint8_t test_data[TEST_DATA_LEN] = {
    0x43, 0x72, 0x79, 0x70, 0x74, 0x6f, 0x67, 0x72, 0x61, 0x70, 0x68, 0x69, 0x63, 0x20, 0x46, 0x6f,
    0x72, 0x75, 0x6d, 0x20, 0x52, 0x65, 0x73, 0x65, 0x61, 0x72, 0x63, 0x68, 0x20, 0x47, 0x72, 0x6f,
    0x75, 0x70, 0x20, 0x2d, 0x20, 0x50, 0x6f, 0x6c, 0x79, 0x31, 0x33, 0x30, 0x35, 0x20, 0x4d, 0x41,
    0x43, 0x20, 0x62, 0x65, 0x6e, 0x63, 0x68, 0x6d, 0x61, 0x72, 0x6b, 0x20, 0x74, 0x65, 0x73, 0x74,
    0x20, 0x64, 0x61, 0x74, 0x61, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x6d,
    0x69, 0x74, 0x74, 0x65, 0x6e, 0x74, 0x20, 0x63, 0x6f, 0x6d, 0x70, 0x75, 0x74, 0x69, 0x6e, 0x67,
    0x20, 0x73, 0x79, 0x73, 0x74, 0x65, 0x6d, 0x73, 0x20, 0x77, 0x69, 0x74, 0x68, 0x20, 0x65, 0x6e,
    0x65, 0x72, 0x67, 0x79, 0x20, 0x68, 0x61, 0x72, 0x76, 0x65, 0x73, 0x74, 0x69, 0x6e, 0x67, 0x2e};

/* --- Macros --- */

#define CONSTANT_TIME_CARRY(a, b) ((a ^ ((a ^ b) | ((a - b) ^ b))) >> (sizeof(a) * 8 - 1))

/* --- Helper Functions --- */

/* Pick 32-bit integer in little endian order */
FORCE_INLINE uint32_t U8TOU32(const uint8_t *p) {
    return (((uint32_t)(p[0] & 0xff)) | ((uint32_t)(p[1] & 0xff) << 8) |
            ((uint32_t)(p[2] & 0xff) << 16) | ((uint32_t)(p[3] & 0xff) << 24));
}

/* Store a 32-bit integer in little endian */
FORCE_INLINE void U32TO8(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v) & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

FORCE_INLINE void poly1305_init(poly1305_ctx_t *ctx, const uint8_t k[32]) {
    poly1305_internal *st = &ctx->st;

    /* h = 0 */
    st->h[0] = 0;
    st->h[1] = 0;
    st->h[2] = 0;
    st->h[3] = 0;
    st->h[4] = 0;

    /* r &= 0x0ffffffc0ffffffc0ffffffc0fffffff */
    st->r[0] = U8TOU32(&k[0]) & 0x0fffffff;
    st->r[1] = U8TOU32(&k[4]) & 0x0ffffffc;
    st->r[2] = U8TOU32(&k[8]) & 0x0ffffffc;
    st->r[3] = U8TOU32(&k[12]) & 0x0ffffffc;

    /* Store nonce */
    ctx->nonce[0] = U8TOU32(&k[16]);
    ctx->nonce[1] = U8TOU32(&k[20]);
    ctx->nonce[2] = U8TOU32(&k[24]);
    ctx->nonce[3] = U8TOU32(&k[28]);

    ctx->num = 0;
}

FORCE_INLINE void poly1305_blocks(poly1305_internal *st, const uint8_t *inp, uint32_t len,
                                  uint32_t padbit) {
    uint32_t r0, r1, r2, r3;
    uint32_t s1, s2, s3;
    uint32_t h0, h1, h2, h3, h4, c;
    uint64_t d0, d1, d2, d3;

    r0 = st->r[0];
    r1 = st->r[1];
    r2 = st->r[2];
    r3 = st->r[3];

    s1 = r1 + (r1 >> 2);
    s2 = r2 + (r2 >> 2);
    s3 = r3 + (r3 >> 2);

    h0 = st->h[0];
    h1 = st->h[1];
    h2 = st->h[2];
    h3 = st->h[3];
    h4 = st->h[4];

    while (len >= POLY1305_BLOCK_SIZE) {
        __loop_tripcount(8); /* 128 / 16 = 8 blocks max */

        /* h += m[i] */
        h0 = (uint32_t)(d0 = (uint64_t)h0 + U8TOU32(inp + 0));
        h1 = (uint32_t)(d1 = (uint64_t)h1 + (d0 >> 32) + U8TOU32(inp + 4));
        h2 = (uint32_t)(d2 = (uint64_t)h2 + (d1 >> 32) + U8TOU32(inp + 8));
        h3 = (uint32_t)(d3 = (uint64_t)h3 + (d2 >> 32) + U8TOU32(inp + 12));
        h4 += (uint32_t)(d3 >> 32) + padbit;

        /* h *= r "%" p */
        d0 = ((uint64_t)h0 * r0) + ((uint64_t)h1 * s3) + ((uint64_t)h2 * s2) + ((uint64_t)h3 * s1);
        d1 = ((uint64_t)h0 * r1) + ((uint64_t)h1 * r0) + ((uint64_t)h2 * s3) + ((uint64_t)h3 * s2) +
             (h4 * s1);
        d2 = ((uint64_t)h0 * r2) + ((uint64_t)h1 * r1) + ((uint64_t)h2 * r0) + ((uint64_t)h3 * s3) +
             (h4 * s2);
        d3 = ((uint64_t)h0 * r3) + ((uint64_t)h1 * r2) + ((uint64_t)h2 * r1) + ((uint64_t)h3 * r0) +
             (h4 * s3);
        h4 = (h4 * r0);

        /* Reduction */
        h0 = (uint32_t)d0;
        h1 = (uint32_t)(d1 += d0 >> 32);
        h2 = (uint32_t)(d2 += d1 >> 32);
        h3 = (uint32_t)(d3 += d2 >> 32);
        h4 += (uint32_t)(d3 >> 32);

        c = (h4 >> 2) + (h4 & ~3U);
        h4 &= 3;
        h0 += c;
        h1 += (c = CONSTANT_TIME_CARRY(h0, c));
        h2 += (c = CONSTANT_TIME_CARRY(h1, c));
        h3 += (c = CONSTANT_TIME_CARRY(h2, c));
        h4 += CONSTANT_TIME_CARRY(h3, c);

        inp += POLY1305_BLOCK_SIZE;
        len -= POLY1305_BLOCK_SIZE;
    }

    st->h[0] = h0;
    st->h[1] = h1;
    st->h[2] = h2;
    st->h[3] = h3;
    st->h[4] = h4;
}

FORCE_INLINE void poly1305_emit(poly1305_internal *st, uint8_t mac[16], const uint32_t nonce[4]) {
    uint32_t h0, h1, h2, h3, h4;
    uint32_t g0, g1, g2, g3, g4;
    uint64_t t;
    uint32_t mask;

    h0 = st->h[0];
    h1 = st->h[1];
    h2 = st->h[2];
    h3 = st->h[3];
    h4 = st->h[4];

    /* Compare to modulus by computing h + -p */
    g0 = (uint32_t)(t = (uint64_t)h0 + 5);
    g1 = (uint32_t)(t = (uint64_t)h1 + (t >> 32));
    g2 = (uint32_t)(t = (uint64_t)h2 + (t >> 32));
    g3 = (uint32_t)(t = (uint64_t)h3 + (t >> 32));
    g4 = h4 + (uint32_t)(t >> 32);

    /* If there was carry into 131st bit, h3:h0 = g3:g0 */
    mask = 0 - (g4 >> 2);
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;

    /* mac = (h + nonce) % (2^128) */
    h0 = (uint32_t)(t = (uint64_t)h0 + nonce[0]);
    h1 = (uint32_t)(t = (uint64_t)h1 + (t >> 32) + nonce[1]);
    h2 = (uint32_t)(t = (uint64_t)h2 + (t >> 32) + nonce[2]);
    h3 = (uint32_t)(t = (uint64_t)h3 + (t >> 32) + nonce[3]);

    U32TO8(mac + 0, h0);
    U32TO8(mac + 4, h1);
    U32TO8(mac + 8, h2);
    U32TO8(mac + 12, h3);
}

FORCE_INLINE void poly1305_update(poly1305_ctx_t *ctx, const uint8_t *inp, uint32_t len) {
    uint32_t rem, num;

    if ((num = ctx->num)) {
        rem = POLY1305_BLOCK_SIZE - num;
        if (len >= rem) {
            __builtin_memcpy(ctx->data + num, inp, rem);
            poly1305_blocks(&ctx->st, ctx->data, POLY1305_BLOCK_SIZE, 1);
            inp += rem;
            len -= rem;
        } else {
            __builtin_memcpy(ctx->data + num, inp, len);
            ctx->num = num + len;
            return;
        }
    }

    rem = len % POLY1305_BLOCK_SIZE;
    len -= rem;

    if (len >= POLY1305_BLOCK_SIZE) {
        poly1305_blocks(&ctx->st, inp, len, 1);
        inp += len;
    }

    if (rem)
        __builtin_memcpy(ctx->data, inp, rem);

    ctx->num = rem;
}

FORCE_INLINE void poly1305_final(poly1305_ctx_t *ctx, uint8_t mac[16]) {
    uint32_t num;

    if ((num = ctx->num)) {
        ctx->data[num++] = 1; /* pad bit */
        while (num < POLY1305_BLOCK_SIZE) {
            __loop_tripcount(16); /* at most 15 iterations */
            ctx->data[num++] = 0;
        }
        poly1305_blocks(&ctx->st, ctx->data, POLY1305_BLOCK_SIZE, 0);
    }

    poly1305_emit(&ctx->st, mac, ctx->nonce);
}

/* --- Main --- */

__attribute__((noinline)) int main(void) {
    BENCH_INIT();
    uint32_t iter, i;

    for (i = 0; i < TEST_DATA_LEN; i++) {
        __loop_tripcount(TEST_DATA_LEN);
        g_msg[i] = test_data[i];
    }

    /* MAC a chain of messages: each MAC is folded back into the next message. */
    for (iter = 0; iter < NUM_MESSAGES; iter++) {
        __loop_tripcount(NUM_MESSAGES);
        poly1305_init(&g_ctx, key);
        poly1305_update(&g_ctx, g_msg, TEST_DATA_LEN);
        poly1305_final(&g_ctx, g_hash);
        for (i = 0; i < 16; i++) {
            __loop_tripcount(16);
            g_msg[i] ^= g_hash[i];
        }
    }

    BENCH_EXIT((int)g_hash[0]);
    return (int)g_hash[0];
}
