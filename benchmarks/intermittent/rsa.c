#include "benchmark.h"
#include "loop_tripcount.h"
#include <stdbool.h>
#include <stdint.h>

#define FORCE_INLINE static inline __attribute__((always_inline))

// --- Configuration & Constants ---

#define DIGIT_BITS 8
#define DIGIT_MASK 0x00ff
#include "./data/keysize.h"

#define NUM_DIGITS (KEY_SIZE_BITS / DIGIT_BITS)
#define PRINT_HEX_ASCII_COLS 8

/** @brief Type large enough to store a product of two digits */
typedef uint16_t digit_t;

typedef struct {
    uint8_t n[NUM_DIGITS]; // modulus
    digit_t e;             // exponent
} pubkey_t;

static const uint8_t PAD_DIGITS[] = {0x01};
#define NUM_PAD_DIGITS (sizeof(PAD_DIGITS) / sizeof(PAD_DIGITS[0]))

// modulus: byte order: LSB to MSB, constraint MSB>=0x80
static const pubkey_t pubkey = {
#include "./data/key.txt"
};

static const unsigned char PLAINTEXT[] =
#include "./data/plaintext.txt"
    ;

#define NUM_PLAINTEXT_BLOCKS (sizeof(PLAINTEXT) / (NUM_DIGITS - NUM_PAD_DIGITS) + 1)
#define CYPHERTEXT_SIZE (NUM_PLAINTEXT_BLOCKS * NUM_DIGITS)

// --- Global Buffers ---
digit_t g_A[NUM_DIGITS];
digit_t g_B[NUM_DIGITS];
digit_t g_product[NUM_DIGITS * 2];
digit_t g_base[NUM_DIGITS * 2];
digit_t g_block[NUM_DIGITS * 2];
digit_t g_cyphertext[CYPHERTEXT_SIZE];
unsigned g_cyphertext_len = 0;

// --- Helper Functions ---

// Helper for 16-bit multiplication used in reduction
FORCE_INLINE uint32_t mult16(digit_t a, digit_t b) {
    return (uint32_t)a * b;
}

// --- Logic Functions ---

// Forward declaration
FORCE_INLINE void mult_mod_operation(digit_t *A, digit_t *B, digit_t *result_buffer);

/* * Performs: result = (A * B) mod N
 * This consolidates task_mult_mod, task_mult, and all task_reduce_*
 */
FORCE_INLINE void mult_mod_operation(digit_t *A, digit_t *B, digit_t *result_buffer) {
    int i, j;

    // Standard schoolbook multiplication: A * B -> product
    digit_t c = 0;
    digit_t p = 0;

    // Clear product buffer first
    for (i = 0; i < NUM_DIGITS * 2; i++) {
        __loop_tripcount(NUM_DIGITS * 2); // 2 * NUM_DIGITS iterations
        g_product[i] = 0;
    }

    for (int digit = 0; digit < NUM_DIGITS * 2; ++digit) {
        __loop_tripcount(NUM_DIGITS * 2); // 2 * NUM_DIGITS iterations
        p = c;                            // carry from previous
        c = 0;                            // new carry

        for (i = 0; i < NUM_DIGITS; ++i) {
            __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
            if (digit - i >= 0 && digit - i < NUM_DIGITS) {
                digit_t a_val = A[digit - i];
                digit_t b_val = B[i];
                uint32_t dp = (uint32_t)a_val * b_val;

                // Add to current accumulator
                p += (dp & DIGIT_MASK);
                // Calculate carry
                c += (dp >> DIGIT_BITS);
            }
        }

        // Handle local accumulator overflow
        c += (p >> DIGIT_BITS);
        p &= DIGIT_MASK;

        g_product[digit] = p;
    }

    // Find Most Significant Digit (MSD)
    int d = 2 * NUM_DIGITS;
    digit_t m;
    do {
        __loop_tripcount(NUM_DIGITS * 2); // at most 2 * NUM_DIGITS iterations
        d--;
        m = g_product[d];
    } while (m == 0 && d > 0);

    // If result is 0, we are done
    if (m == 0) {
        for (i = 0; i < NUM_DIGITS; i++) {
            __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
            result_buffer[i] = 0;
        }
        return;
    }

    // Reduction Loop
    while (1) {
        __loop_tripcount(NUM_DIGITS *
                         3); // conservative bound: up to 3 * NUM_DIGITS reduction steps

        bool normalizable = true;
        unsigned offset = d + 1 - NUM_DIGITS;

        for (i = d; i >= 0; --i) {
            __loop_tripcount(NUM_DIGITS * 2); // at most 2 * NUM_DIGITS iterations
            digit_t m_val = g_product[i];
            digit_t n_val = (i - offset >= 0 && i - offset < NUM_DIGITS) ? pubkey.n[i - offset] : 0;

            if (m_val > n_val) {
                break;
            } else if (m_val < n_val) {
                normalizable = false;
                break;
            }
        }

        if (!normalizable && d == NUM_DIGITS - 1) {
            // Reduction done: message < modulus
            break;
        }

        if (normalizable) {
            // Simple subtraction: product = product - (N << offset)
            unsigned borrow = 0;
            for (i = 0; i < NUM_DIGITS; ++i) {
                __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
                digit_t m_val = g_product[i + offset];
                digit_t n_val = pubkey.n[i];
                digit_t s = n_val + borrow;

                if (m_val < s) {
                    m_val += (1 << DIGIT_BITS);
                    borrow = 1;
                } else {
                    borrow = 0;
                }
                g_product[i + offset] = m_val - s;
            }

            if (offset == 0) {
                break;
            }
        } else {
            digit_t n1 = pubkey.n[NUM_DIGITS - 1];
            digit_t n0 = pubkey.n[NUM_DIGITS - 2];
            digit_t n_div = ((n1 << DIGIT_BITS) + n0);

            digit_t m2 = g_product[d];
            digit_t m1 = g_product[d - 1];
            digit_t m0 = g_product[d - 2];
            digit_t m_n = pubkey.n[NUM_DIGITS - 1];

            digit_t q;
            if (m2 == m_n) {
                q = (1 << DIGIT_BITS) - 1;
            } else {
                q = ((m2 << DIGIT_BITS) + m1) / m_n;
            }

            uint32_t n_q = ((uint32_t)m2 << (2 * DIGIT_BITS)) + (m1 << DIGIT_BITS) + m0;
            digit_t q_final = q + 1;
            uint32_t qn;

            do {
                __loop_tripcount(4); // quotient refinement, typically 1-2 steps
                q_final--;
                qn = mult16(n_div, q_final);
            } while (qn > n_q);

            unsigned current_d = d;
            d--;

            digit_t qn_arr[NUM_DIGITS * 2];
            for (int k = 0; k < NUM_DIGITS * 2; k++) {
                __loop_tripcount(NUM_DIGITS * 2); // 2 * NUM_DIGITS iterations
                qn_arr[k] = 0;
            }

            unsigned mul_offset = current_d - NUM_DIGITS;
            digit_t mul_c = 0;

            for (i = mul_offset; i < 2 * NUM_DIGITS; ++i) {
                __loop_tripcount(NUM_DIGITS * 2); // at most 2 * NUM_DIGITS iterations
                digit_t m_curr = mul_c;
                if (i < mul_offset + NUM_DIGITS) {
                    m_curr += q_final * pubkey.n[i - mul_offset];
                }
                mul_c = m_curr >> DIGIT_BITS;
                qn_arr[i] = m_curr & DIGIT_MASK;
            }

            char relation = '=';
            for (i = NUM_DIGITS * 2 - 1; i >= 0; --i) {
                __loop_tripcount(NUM_DIGITS * 2); // 2 * NUM_DIGITS iterations
                if (g_product[i] > qn_arr[i]) {
                    relation = '>';
                    break;
                } else if (g_product[i] < qn_arr[i]) {
                    relation = '<';
                    break;
                }
            }

            if (relation == '<') {
                unsigned add_offset = current_d - NUM_DIGITS;
                digit_t add_c = 0;
                for (i = add_offset; i < 2 * NUM_DIGITS; ++i) {
                    __loop_tripcount(NUM_DIGITS * 2); // at most 2 * NUM_DIGITS iterations
                    digit_t add_n = (i < add_offset + NUM_DIGITS) ? pubkey.n[i - add_offset] : 0;
                    digit_t add_r = add_c + g_product[i] + add_n;
                    add_c = add_r >> DIGIT_BITS;
                    g_product[i] = add_r & DIGIT_MASK;
                }
            }

            unsigned sub_offset = current_d - NUM_DIGITS;
            unsigned sub_borrow = 0;

            for (i = 0; i < 2 * NUM_DIGITS; ++i) {
                __loop_tripcount(NUM_DIGITS * 2); // 2 * NUM_DIGITS iterations
                if (i >= sub_offset) {
                    digit_t sub_qn = qn_arr[i];
                    digit_t sub_s = sub_qn + sub_borrow;

                    if (g_product[i] < sub_s) {
                        g_product[i] += (1 << DIGIT_BITS);
                        sub_borrow = 1;
                    } else {
                        sub_borrow = 0;
                    }
                    g_product[i] = g_product[i] - sub_s;
                }
            }
        }
    }

    // Copy result to output buffer
    for (i = 0; i < NUM_DIGITS; i++) {
        __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
        result_buffer[i] = g_product[i];
    }
}

__attribute__((noinline)) int main(void) {
    BENCH_INIT();
    unsigned message_length = sizeof(PLAINTEXT) - 1;
    unsigned block_offset = 0;

    // Main Loop handling blocks
    while (block_offset < message_length) {
        __loop_tripcount(NUM_PLAINTEXT_BLOCKS); // up to NUM_PLAINTEXT_BLOCKS plaintext blocks

        // Construct the base for this block
        int i;
        for (i = 0; i < NUM_DIGITS - NUM_PAD_DIGITS; ++i) {
            __loop_tripcount(NUM_DIGITS); // up to NUM_DIGITS - NUM_PAD_DIGITS iterations
            g_base[i] = (block_offset + i < message_length) ? PLAINTEXT[block_offset + i] : 0xFF;
        }
        for (int j = 0; i < NUM_DIGITS; ++i, ++j) {
            __loop_tripcount(NUM_PAD_DIGITS); // NUM_PAD_DIGITS iterations
            g_base[i] = PAD_DIGITS[j];
        }

        // Initialize block (which accumulates result) to 1
        g_block[0] = 1;
        for (i = 1; i < NUM_DIGITS; ++i) {
            __loop_tripcount(NUM_DIGITS); // NUM_DIGITS - 1 iterations
            g_block[i] = 0;
        }

        digit_t e = pubkey.e;
        block_offset += NUM_DIGITS - NUM_PAD_DIGITS;

        // Modular Exponentiation
        while (e > 0) {
            __loop_tripcount(16); // up to 16 exponent bits (digit_t is uint16_t)
            bool multiply = e & 0x1;
            e >>= 1;

            if (multiply) {
                // block = (block * base) % N
                for (int k = 0; k < NUM_DIGITS; k++) {
                    __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
                    g_A[k] = g_base[k];
                }
                for (int k = 0; k < NUM_DIGITS; k++) {
                    __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
                    g_B[k] = g_block[k];
                }

                mult_mod_operation(g_A, g_B, g_block);
            }

            if (e > 0) {
                // base = (base * base) % N
                for (int k = 0; k < NUM_DIGITS; k++) {
                    __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
                    g_A[k] = g_base[k];
                }
                for (int k = 0; k < NUM_DIGITS; k++) {
                    __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
                    g_B[k] = g_base[k];
                }

                mult_mod_operation(g_A, g_B, g_base);
            }
        }

        // Save block to cyphertext
        if (g_cyphertext_len + NUM_DIGITS <= CYPHERTEXT_SIZE) {
            for (i = 0; i < NUM_DIGITS; ++i) {
                __loop_tripcount(NUM_DIGITS); // NUM_DIGITS iterations
                g_cyphertext[g_cyphertext_len++] = g_block[i];
            }
        }
    }

    BENCH_EXIT((int)g_cyphertext_len);
    return (int)g_cyphertext_len;
}
