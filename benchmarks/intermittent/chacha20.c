/*
 * ChaCha20 stream cipher - preprocessed for MILP checkpoint insertion.
 *
 * Based on: https://github.com/marcizhu/ChaCha20
 * Original Copyright (C) 2022 Marc Izquierdo (MIT License)
 *
 * Single 64-byte block encryption for intermittent computing benchmark.
 */

#include "debug_counters.h"
#include "loop_tripcount.h"
#include <stdint.h>

#define FORCE_INLINE static inline __attribute__((always_inline))

#define INPUT_SIZE 64

/* --- ChaCha20 types --- */

typedef uint8_t key256_t[32];
typedef uint8_t nonce96_t[12];

typedef struct {
    uint32_t state[16];
} ChaCha20_Ctx;

/* --- Mutable globals --- */

ChaCha20_Ctx ctx;
uint8_t enc_output[INPUT_SIZE] __attribute__((section(".fram")));

/* --- Const data (no annotation) --- */

static const uint8_t test_data[INPUT_SIZE] = {
    0x43, 0x68, 0x61, 0x43, 0x68, 0x61, 0x32, 0x30, 0x20, 0x74, 0x65, 0x73, 0x74, 0x20, 0x64, 0x61,
    0x74, 0x61, 0x20, 0x66, 0x6f, 0x72, 0x20, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x6d, 0x69, 0x74, 0x74,
    0x65, 0x6e, 0x74, 0x20, 0x63, 0x6f, 0x6d, 0x70, 0x75, 0x74, 0x69, 0x6e, 0x67, 0x20, 0x62, 0x65,
    0x6e, 0x63, 0x68, 0x6d, 0x61, 0x72, 0x6b, 0x2e, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

/* --- ChaCha20 helper macros --- */

#define CHACHA20_CONSTANT "expand 32-byte k"
#define CHACHA20_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define CHACHA20_QR(a, b, c, d)                                                                    \
    a += b;                                                                                        \
    d ^= a;                                                                                        \
    d = CHACHA20_ROTL(d, 16);                                                                      \
    c += d;                                                                                        \
    b ^= c;                                                                                        \
    b = CHACHA20_ROTL(b, 12);                                                                      \
    a += b;                                                                                        \
    d ^= a;                                                                                        \
    d = CHACHA20_ROTL(d, 8);                                                                       \
    c += d;                                                                                        \
    b ^= c;                                                                                        \
    b = CHACHA20_ROTL(b, 7)

/* --- ChaCha20 helper functions --- */

FORCE_INLINE uint32_t pack4(const uint8_t *a) {
    uint32_t res =
        (uint32_t)a[0] << 0 | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24;
    return res;
}

FORCE_INLINE void ChaCha20_block_next(const uint32_t in[16], uint32_t out[16],
                                      uint8_t **keystream) {
    int i;
    for (i = 0; i < 16; i++) {
        __loop_tripcount(16);
        out[i] = in[i];
    }

    /* 10 rounds (unrolled, no loop) */
    /* Round 1/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 2/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 3/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 4/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 5/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 6/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 7/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 8/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 9/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Round 10/10 */
    CHACHA20_QR(out[0], out[4], out[8], out[12]);
    CHACHA20_QR(out[1], out[5], out[9], out[13]);
    CHACHA20_QR(out[2], out[6], out[10], out[14]);
    CHACHA20_QR(out[3], out[7], out[11], out[15]);
    CHACHA20_QR(out[0], out[5], out[10], out[15]);
    CHACHA20_QR(out[1], out[6], out[11], out[12]);
    CHACHA20_QR(out[2], out[7], out[8], out[13]);
    CHACHA20_QR(out[3], out[4], out[9], out[14]);

    /* Add back input */
    for (i = 0; i < 16; i++) {
        __loop_tripcount(16);
        out[i] += in[i];
    }

    if (keystream != (void *)0)
        *keystream = (uint8_t *)out;
}

FORCE_INLINE void ChaCha20_init(ChaCha20_Ctx *c, const key256_t key, const nonce96_t nonce,
                                uint32_t count) {
    c->state[0] = pack4((const uint8_t *)CHACHA20_CONSTANT + 0 * 4);
    c->state[1] = pack4((const uint8_t *)CHACHA20_CONSTANT + 1 * 4);
    c->state[2] = pack4((const uint8_t *)CHACHA20_CONSTANT + 2 * 4);
    c->state[3] = pack4((const uint8_t *)CHACHA20_CONSTANT + 3 * 4);
    c->state[4] = pack4(key + 0 * 4);
    c->state[5] = pack4(key + 1 * 4);
    c->state[6] = pack4(key + 2 * 4);
    c->state[7] = pack4(key + 3 * 4);
    c->state[8] = pack4(key + 4 * 4);
    c->state[9] = pack4(key + 5 * 4);
    c->state[10] = pack4(key + 6 * 4);
    c->state[11] = pack4(key + 7 * 4);
    c->state[12] = count;
    c->state[13] = pack4(nonce + 0 * 4);
    c->state[14] = pack4(nonce + 1 * 4);
    c->state[15] = pack4(nonce + 2 * 4);
}

FORCE_INLINE void ChaCha20_xor(ChaCha20_Ctx *c, const uint8_t *input_buffer, uint8_t *output_buffer,
                               const uint32_t bufflen) {
    uint32_t tmp[16];
    uint8_t *keystream = (uint8_t *)0;
    uint32_t i, j;

    for (i = 0; i < bufflen; i += 64) {
        __loop_tripcount(INPUT_SIZE / 64); /* 1 iteration for 64-byte input */

        ChaCha20_block_next(c->state, tmp, &keystream);
        c->state[12]++;

        if (c->state[12] == 0) {
            c->state[13]++;
        }

        for (j = i; j < i + 64; j++) {
            __loop_tripcount(64);
            if (j >= bufflen)
                break;
            output_buffer[j] = input_buffer[j] ^ keystream[j - i];
        }
    }
}

/* --- Main --- */

__attribute__((noinline)) int main(void) {
    DEBUG_INIT();
    const uint8_t key[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
                             0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
                             0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    const uint8_t nonce[12] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x4a, 0x00, 0x00, 0x00, 0x00};
    int i;

    ChaCha20_init(&ctx, key, nonce, 1);

    /* Copy test data to output buffer */
    for (i = 0; i < INPUT_SIZE; i++) {
        __loop_tripcount(INPUT_SIZE);
        enc_output[i] = test_data[i];
    }

    /* Encrypt */
    ChaCha20_xor(&ctx, enc_output, enc_output, INPUT_SIZE);

    DEBUG_EXIT((int)enc_output[0]);
    return (int)enc_output[0];
}
