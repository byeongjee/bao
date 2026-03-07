/*
 * FFT - Preprocessed benchmark for intermittent computing.
 * Based on Don Cross FFT implementation (public domain) from MiBench/ulswap-bench.
 * Merged into single file: fftmisc.c + fourierf.c + main.c + common RNG.
 */
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include "loop_tripcount.h"

#define DDC_PI  (3.14159265358979323846)
#define TRUE  1
#define FALSE 0
#define BITS_PER_WORD (sizeof(uint32_t) * 8)

#define FORCE_INLINE static inline __attribute__((always_inline))

#define MAXSIZE  256
#define MAXWAVES 4

/* --- Mutable globals (milp_candidate) --- */

static float realin[MAXSIZE]  __attribute__((annotate("milp_candidate")));
static float imagin[MAXSIZE]  __attribute__((annotate("milp_candidate")));
static float realout[MAXSIZE] __attribute__((annotate("milp_candidate")));
static float imagout[MAXSIZE] __attribute__((annotate("milp_candidate")));
static float Coeff[MAXWAVES]  __attribute__((annotate("milp_candidate")));
static float Amp[MAXWAVES]    __attribute__((annotate("milp_candidate")));

/* --- Simple LCG RNG (from ulswap-bench common.c) --- */

static uint32_t _myrand_seed __attribute__((annotate("milp_candidate"))) = 1234;

FORCE_INLINE void my_srand(uint32_t new_seed)
{
    _myrand_seed = new_seed;
}

FORCE_INLINE uint32_t my_rand(void)
{
    _myrand_seed = (uint32_t)(1103515245 * _myrand_seed + 12345);
    return _myrand_seed;
}

/* --- FFT helper routines (from fftmisc.c) --- */

FORCE_INLINE int32_t IsPowerOfTwo(uint32_t x)
{
    if (x < 2)
        return FALSE;

    if (x & (x - 1))
        return FALSE;

    return TRUE;
}

FORCE_INLINE uint32_t NumberOfBitsNeeded(uint32_t PowerOfTwo)
{
    uint32_t i;

    for (i = 0;; i++)
    {
        __loop_tripcount(32);  /* at most 32 bits */
        if (PowerOfTwo & (1 << i))
            return i;
    }
}

FORCE_INLINE uint32_t ReverseBits(uint32_t index, uint32_t NumBits)
{
    uint32_t i, rev;

    for (i = rev = 0; i < NumBits; i++)
    {
        __loop_tripcount(8);  /* log2(MAXSIZE) = 8 */
        rev = (rev << 1) | (index & 1);
        index >>= 1;
    }

    return rev;
}

/* --- FFT core (from fourierf.c) --- */

FORCE_INLINE void fft_float(
    uint32_t NumSamples,
    int32_t  InverseTransform,
    float    *RealIn,
    float    *ImagIn,
    float    *RealOut,
    float    *ImagOut)
{
    uint32_t NumBits;
    uint32_t i, j, k, n;
    uint32_t BlockSize, BlockEnd;

    double angle_numerator = 2.0 * DDC_PI;
    double tr, ti;

    if (InverseTransform)
        angle_numerator = -angle_numerator;

    NumBits = NumberOfBitsNeeded(NumSamples);

    /* Simultaneous data copy and bit-reversal ordering into outputs */
    for (i = 0; i < NumSamples; i++)
    {
        __loop_tripcount(MAXSIZE);  /* 256 */
        j = ReverseBits(i, NumBits);
        RealOut[j] = RealIn[i];
        ImagOut[j] = (ImagIn == NULL) ? 0.0f : ImagIn[i];
    }

    /* FFT butterfly */
    BlockEnd = 1;
    for (BlockSize = 2; BlockSize <= NumSamples; BlockSize <<= 1)
    {
        __loop_tripcount(8);  /* log2(MAXSIZE) = 8 */
        double delta_angle = angle_numerator / (double)BlockSize;
        double sm2 = sin(-2 * delta_angle);
        double sm1 = sin(-delta_angle);
        double cm2 = cos(-2 * delta_angle);
        double cm1 = cos(-delta_angle);
        double w = 2 * cm1;
        double ar[3], ai[3];

        for (i = 0; i < NumSamples; i += BlockSize)
        {
            __loop_tripcount(MAXSIZE);  /* at most 256 */
            ar[2] = cm2;
            ar[1] = cm1;

            ai[2] = sm2;
            ai[1] = sm1;

            for (j = i, n = 0; n < BlockEnd; j++, n++)
            {
                __loop_tripcount(MAXSIZE / 2);  /* at most 128 */
                ar[0] = w * ar[1] - ar[2];
                ar[2] = ar[1];
                ar[1] = ar[0];

                ai[0] = w * ai[1] - ai[2];
                ai[2] = ai[1];
                ai[1] = ai[0];

                k = j + BlockEnd;
                tr = ar[0] * RealOut[k] - ai[0] * ImagOut[k];
                ti = ar[0] * ImagOut[k] + ai[0] * RealOut[k];

                RealOut[k] = RealOut[j] - tr;
                ImagOut[k] = ImagOut[j] - ti;

                RealOut[j] += tr;
                ImagOut[j] += ti;
            }
        }

        BlockEnd = BlockSize;
    }

    /* Normalize if inverse transform */
    if (InverseTransform)
    {
        double denom = (double)NumSamples;

        for (i = 0; i < NumSamples; i++)
        {
            __loop_tripcount(MAXSIZE);  /* 256 */
            RealOut[i] /= denom;
            ImagOut[i] /= denom;
        }
    }
}

/* --- Main benchmark entry point --- */

int main(void)
{
    uint32_t i, j;

    my_srand(1);

    /* Makes MAXWAVES waves of random amplitude and period */
    for (i = 0; i < MAXWAVES; i++)
    {
        __loop_tripcount(MAXWAVES);  /* 4 */
        Coeff[i] = my_rand() % 1000;
        Amp[i]   = my_rand() % 1000;
    }
    for (i = 0; i < MAXSIZE; i++)
    {
        __loop_tripcount(MAXSIZE);  /* 256 */
        realin[i] = 0;
        for (j = 0; j < MAXWAVES; j++)
        {
            __loop_tripcount(MAXWAVES);  /* 4 */
            if (my_rand() % 2)
            {
                realin[i] += Coeff[j] * cos(Amp[j] * i);
            }
            else
            {
                realin[i] += Coeff[j] * sin(Amp[j] * i);
            }
            imagin[i] = 0;
        }
    }

    /* Forward FFT */
    fft_float(MAXSIZE, FALSE, realin, imagin, realout, imagout);

    /* Inverse FFT */
    fft_float(MAXSIZE, TRUE, realin, imagin, realout, imagout);

    return 0;
}
