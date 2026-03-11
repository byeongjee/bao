#include "debug_counters.h"
#include "loop_tripcount.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define FORCE_INLINE static inline __attribute__((always_inline))

// Algorithm Constants
#define NUM_WARMUP_SAMPLES 3
#define ACCEL_WINDOW_SIZE 3
#define MODEL_SIZE 16
#define SAMPLE_NOISE_FLOOR 10
#define SAMPLES_TO_COLLECT 64 // Reduced for faster demo loop

static uint16_t lfsr_state;

FORCE_INLINE uint16_t simple_rand(void) {
    // If the last bit is 1, shift and XOR. If 0, just shift.
    // 0xB400 is the tap configuration for a 16-bit maximal-length LFSR
    if (lfsr_state & 1) {
        lfsr_state = (lfsr_state >> 1) ^ 0xB400u;
    } else {
        lfsr_state >>= 1;
    }
    return lfsr_state;
}

// --- Data Types ---

// struct from libadxl362
typedef struct {
    int8_t x;
    int8_t y;
    int8_t z;
} threeAxis_t_8;

typedef threeAxis_t_8 accelReading;
typedef accelReading accelWindow[ACCEL_WINDOW_SIZE];

typedef struct {
    unsigned meanmag;
    unsigned stddevmag;
} features_t;

typedef enum {
    CLASS_STATIONARY,
    CLASS_MOVING,
} class_t;

typedef struct {
    features_t stationary[MODEL_SIZE];
    features_t moving[MODEL_SIZE];
} model_t;

typedef struct {
    unsigned totalCount;
    unsigned movingCount;
    unsigned stationaryCount;
} stats_t;

// --- Helper Functions ---

// Integer Square Root (Replaces libmspmath)
FORCE_INLINE unsigned sqrt16(unsigned long n) {
    unsigned long c = 0x8000;
    unsigned long g = 0x8000;
    for (;;) {
        __loop_tripcount(16); // 16-bit binary search
        if (g * g > n)
            g ^= c;
        c >>= 1;
        if (c == 0)
            return g;
        g |= c;
    }
}

// --- Sensor Abstraction (Mock Data) ---

static int mock_scenario;

FORCE_INLINE void ACCEL_init() {
    // Real sensor init would go here
}

FORCE_INLINE void accel_sample(accelReading *sample) {
    // Generate synthetic data based on current scenario
    if (mock_scenario == 0) {
        // Stationary: Small noise near 0
        sample->x = (simple_rand() % 4) - 2;
        sample->y = (simple_rand() % 4) - 2;
        sample->z = (simple_rand() % 4) - 2;
    } else {
        // Moving: Large spikes
        sample->x = (simple_rand() % 60) - 30;
        sample->y = (simple_rand() % 60) - 30;
        sample->z = (simple_rand() % 60) - 30;
    }
}

// --- Core Algorithm Logic ---

FORCE_INLINE void acquire_window(accelWindow window) {
    accelReading sample;
    unsigned samplesInWindow = 0;

    while (samplesInWindow < ACCEL_WINDOW_SIZE) {
        __loop_tripcount(ACCEL_WINDOW_SIZE); // 3 iterations
        accel_sample(&sample);
        window[samplesInWindow++] = sample;
    }
}

FORCE_INLINE void transform(accelWindow window) {
    unsigned i = 0;
    for (i = 0; i < ACCEL_WINDOW_SIZE; i++) {
        __loop_tripcount(ACCEL_WINDOW_SIZE); // 3 iterations
        accelReading *sample = &window[i];

        // Simple High-pass / Noise gate filter
        if (abs(sample->x) < SAMPLE_NOISE_FLOOR)
            sample->x = 0;
        if (abs(sample->y) < SAMPLE_NOISE_FLOOR)
            sample->y = 0;
        if (abs(sample->z) < SAMPLE_NOISE_FLOOR)
            sample->z = 0;
    }
}

FORCE_INLINE void featurize(volatile features_t *features, accelWindow aWin) {
    long mean_x = 0, mean_y = 0, mean_z = 0;
    long std_x = 0, std_y = 0, std_z = 0;
    int i;

    // Calculate Mean
    for (i = 0; i < ACCEL_WINDOW_SIZE; i++) {
        __loop_tripcount(ACCEL_WINDOW_SIZE); // 3 iterations
        mean_x += aWin[i].x;
        mean_y += aWin[i].y;
        mean_z += aWin[i].z;
    }
    mean_x /= ACCEL_WINDOW_SIZE;
    mean_y /= ACCEL_WINDOW_SIZE;
    mean_z /= ACCEL_WINDOW_SIZE;

    // Calculate Deviation
    for (i = 0; i < ACCEL_WINDOW_SIZE; i++) {
        __loop_tripcount(ACCEL_WINDOW_SIZE); // 3 iterations
        std_x += labs(aWin[i].x - mean_x);
        std_y += labs(aWin[i].y - mean_y);
        std_z += labs(aWin[i].z - mean_z);
    }
    std_x /= ACCEL_WINDOW_SIZE;
    std_y /= ACCEL_WINDOW_SIZE;
    std_z /= ACCEL_WINDOW_SIZE;

    unsigned meanmag = mean_x * mean_x + mean_y * mean_y + mean_z * mean_z;
    unsigned stddevmag = std_x * std_x + std_y * std_y + std_z * std_z;

    features->meanmag = sqrt16(meanmag);
    features->stddevmag = sqrt16(stddevmag);
}

FORCE_INLINE class_t classify(features_t *features, volatile model_t *model) {
    int move_less_error = 0;
    int stat_less_error = 0;
    volatile features_t *model_features;
    int i;

    // Nearest Centroid-ish classification
    for (i = 0; i < MODEL_SIZE; ++i) {
        __loop_tripcount(MODEL_SIZE); // 16 iterations
        model_features = &model->stationary[i];
        long stat_mean_err = labs((long)model_features->meanmag - (long)features->meanmag);
        long stat_sd_err = labs((long)model_features->stddevmag - (long)features->stddevmag);

        model_features = &model->moving[i];
        long move_mean_err = labs((long)model_features->meanmag - (long)features->meanmag);
        long move_sd_err = labs((long)model_features->stddevmag - (long)features->stddevmag);

        if (move_mean_err < stat_mean_err)
            move_less_error++;
        else
            stat_less_error++;

        if (move_sd_err < stat_sd_err)
            move_less_error++;
        else
            stat_less_error++;
    }

    return (move_less_error > stat_less_error) ? CLASS_MOVING : CLASS_STATIONARY;
}

FORCE_INLINE void warmup_sensor() {
    unsigned discarded = 0;
    accelReading sample;
    while (discarded++ < NUM_WARMUP_SAMPLES) {
        __loop_tripcount(NUM_WARMUP_SAMPLES); // 3 iterations
        accel_sample(&sample);
    }
}

FORCE_INLINE void train(volatile features_t *classModel) {
    accelWindow sampleWindow;
    features_t features;
    unsigned i;

    warmup_sensor();

    for (i = 0; i < MODEL_SIZE; ++i) {
        __loop_tripcount(MODEL_SIZE); // 16 iterations
        acquire_window(sampleWindow);
        transform(sampleWindow);
        featurize(&features, sampleWindow);
        classModel[i] = features;
    }
}

FORCE_INLINE unsigned recognize_loop(volatile model_t *model) {
    volatile stats_t stats = {0};
    accelWindow sampleWindow;
    features_t features;
    class_t class;
    unsigned i;

    for (i = 0; i < SAMPLES_TO_COLLECT; ++i) {
        __loop_tripcount(SAMPLES_TO_COLLECT); // 64 iterations
        // Toggle Mock Scenario halfway through
        if (i == SAMPLES_TO_COLLECT / 2) {
            mock_scenario = !mock_scenario;
        }

        acquire_window(sampleWindow);
        transform(sampleWindow);
        featurize(&features, sampleWindow);
        class = classify(&features, model);

        stats.totalCount++;
        if (class == CLASS_MOVING) {
            stats.movingCount++;
        } else {
            stats.stationaryCount++;
        }
    }

    return stats.totalCount;
}

// --- Main ---

// Global model storage
volatile model_t global_model;

__attribute__((noinline)) int main() {
    DEBUG_INIT();
    lfsr_state = 0xACE1u;
    mock_scenario = 0;

    ACCEL_init();

    // 1. Train "Stationary"
    mock_scenario = 0;
    train(global_model.stationary);

    // 2. Train "Moving"
    mock_scenario = 1;
    train(global_model.moving);

    // 3. Recognize
    mock_scenario = 0;
    unsigned total = recognize_loop(&global_model);

    DEBUG_EXIT((int)total);
    return (int)total;
}
