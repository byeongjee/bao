#include "setup.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// --- Hardware & Configuration ---

// LED Definitions for MSP430FR5994 LaunchPad
#define LED1_PIN BIT0 // P1.0 (Red)
#define LED2_PIN BIT1 // P1.1 (Green)

#define SEC_TO_CYCLES CLOCK_HZ

// Algorithm Constants
#define NUM_WARMUP_SAMPLES 3
#define ACCEL_WINDOW_SIZE 3
#define MODEL_SIZE 16
#define SAMPLE_NOISE_FLOOR 10
#define SAMPLES_TO_COLLECT 64 // Reduced for faster demo loop

static uint16_t lfsr_state __attribute__((section(".noinit")));

INLINE uint16_t simple_rand(void) {
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

// --- Helper Functions (UART & Math) ---

// Integer Square Root (Replaces libmspmath)
INLINE unsigned sqrt16(unsigned long n) {
  unsigned long c = 0x8000;
  unsigned long g = 0x8000;
  for (;;) {
    if (g * g > n)
      g ^= c;
    c >>= 1;
    if (c == 0)
      return g;
    g |= c;
  }
}

// --- Sensor Abstraction (Mock Data) ---

// If you have a real ADXL362, you would replace these with actual driver calls.
// For now, we generate fake data to prove the logic works.

volatile static int mock_scenario
    __attribute__((section(".noinit"))); // 0=Stationary, 1=Moving

void ACCEL_init() {
  // Real sensor init would go here
}

void accel_sample(accelReading *sample) {
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

void acquire_window(accelWindow window) {
  accelReading sample;
  unsigned samplesInWindow = 0;

  while (samplesInWindow < ACCEL_WINDOW_SIZE) {
    accel_sample(&sample);
    window[samplesInWindow++] = sample;
  }
}

void transform(accelWindow window) {
  unsigned i = 0;
  for (i = 0; i < ACCEL_WINDOW_SIZE; i++) {
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

void featurize(volatile features_t *features, accelWindow aWin) {
  long mean_x = 0, mean_y = 0, mean_z = 0;
  long std_x = 0, std_y = 0, std_z = 0;
  int i;

  // Calculate Mean
  for (i = 0; i < ACCEL_WINDOW_SIZE; i++) {
    mean_x += aWin[i].x;
    mean_y += aWin[i].y;
    mean_z += aWin[i].z;
  }
  mean_x /= ACCEL_WINDOW_SIZE;
  mean_y /= ACCEL_WINDOW_SIZE;
  mean_z /= ACCEL_WINDOW_SIZE;

  // Calculate Deviation
  for (i = 0; i < ACCEL_WINDOW_SIZE; i++) {
    std_x += abs(aWin[i].x - mean_x);
    std_y += abs(aWin[i].y - mean_y);
    std_z += abs(aWin[i].z - mean_z);
  }
  std_x /= ACCEL_WINDOW_SIZE;
  std_y /= ACCEL_WINDOW_SIZE;
  std_z /= ACCEL_WINDOW_SIZE;

  unsigned meanmag = mean_x * mean_x + mean_y * mean_y + mean_z * mean_z;
  unsigned stddevmag = std_x * std_x + std_y * std_y + std_z * std_z;

  features->meanmag = sqrt16(meanmag);
  features->stddevmag = sqrt16(stddevmag);
}

class_t classify(features_t *features, volatile model_t *model) {
  int move_less_error = 0;
  int stat_less_error = 0;
  volatile features_t *model_features;
  int i;

  // Nearest Centroid-ish classification
  for (i = 0; i < MODEL_SIZE; ++i) {
    model_features = &model->stationary[i];
    long stat_mean_err =
        abs((long)model_features->meanmag - (long)features->meanmag);
    long stat_sd_err =
        abs((long)model_features->stddevmag - (long)features->stddevmag);

    model_features = &model->moving[i];
    long move_mean_err =
        abs((long)model_features->meanmag - (long)features->meanmag);
    long move_sd_err =
        abs((long)model_features->stddevmag - (long)features->stddevmag);

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

void warmup_sensor() {
  unsigned discarded = 0;
  accelReading sample;
  DEBUG_OUT_STR("Warmup...\n");
  while (discarded++ < NUM_WARMUP_SAMPLES) {
    accel_sample(&sample);
  }
}

void train(volatile features_t *classModel) {
  accelWindow sampleWindow;
  features_t features;
  unsigned i;

  warmup_sensor();

  for (i = 0; i < MODEL_SIZE; ++i) {
    acquire_window(sampleWindow);
    transform(sampleWindow);
    featurize(&features, sampleWindow);
    classModel[i] = features;

#ifdef DEBUG
    // Blink LED1 during training
    P1OUT ^= LED1_PIN;
    delay(SEC_TO_CYCLES / 20);
#endif
  }
  P1OUT &= ~LED1_PIN; // LED off
  DEBUG_OUT_STR("Train done. MeanMag: ");
  DEBUG_OUT_U16(features.meanmag);
  DEBUG_OUT_STR(" StdMag: ");
  DEBUG_OUT_U16(features.stddevmag);
  DEBUG_OUT_CHAR('\n');
}

void recognize_loop(volatile model_t *model) {
  volatile stats_t stats = {0};
  accelWindow sampleWindow;
  features_t features;
  class_t class;
  unsigned i;

  DEBUG_OUT_STR("Starting Recognition Loop...\n");

  for (i = 0; i < SAMPLES_TO_COLLECT; ++i) {
    // Toggle Mock Scenario halfway through to prove it works
    if (i == SAMPLES_TO_COLLECT / 2) {
      DEBUG_OUT_STR("\n--- SWITCHING MOCK MOVEMENT ---\n");
      mock_scenario = !mock_scenario;
    }

    acquire_window(sampleWindow);
    transform(sampleWindow);
    featurize(&features, sampleWindow);
    class = classify(&features, model);

    stats.totalCount++;
    if (class == CLASS_MOVING) {
      stats.movingCount++;
#ifdef DEBUG
      P1OUT |= LED1_PIN; // Red for Moving
      P1OUT &= ~LED2_PIN;
#endif
    } else {
      stats.stationaryCount++;
#ifdef DEBUG
      P1OUT |= LED2_PIN; // Green for Stationary
      P1OUT &= ~LED1_PIN;
#endif
    }

    // Brief delay so we can see the LEDs toggle
#ifdef DEBUG
    delay(SEC_TO_CYCLES / 10);
#endif
  }

  DEBUG_OUT_STR("\nStats: Stationary: ");
  DEBUG_OUT_U16(stats.stationaryCount);
  DEBUG_OUT_STR(" | Moving: ");
  DEBUG_OUT_U16(stats.movingCount);
  DEBUG_OUT_STR(" | Total: ");
  DEBUG_OUT_U16(stats.totalCount);
  DEBUG_OUT_CHAR('\n');
}

// --- Main ---

// Global model storage (in RAM for this simple version)
volatile model_t global_model __attribute__((section(".noinit")));

int main() {
  initialize();
  // LED Setup
  P1DIR |= (LED1_PIN | LED2_PIN);
  P1OUT &= ~(LED1_PIN | LED2_PIN);

  ACCEL_init();

  __enable_interrupt();
  lfsr_state = 0xACE1u;
  mock_scenario = 0;

  begin_measurement_window();

  DEBUG_OUT_STR("\n\n--- Activity Recognition Demo ---\n");

  // 1. Train "Stationary"
  // We set mock_scenario to 0 (Stationary)
  DEBUG_OUT_STR("\n[Mode] Training Stationary Class...\n");
  mock_scenario = 0;
  begin_event();
  train(global_model.stationary);
  end_event();
  delay(SEC_TO_CYCLES);

  // 2. Train "Moving"
  // We set mock_scenario to 1 (Moving)
  DEBUG_OUT_STR("\n[Mode] Training Moving Class...\n");
  mock_scenario = 1;
  begin_event();
  train(global_model.moving);
  end_event();
  delay(SEC_TO_CYCLES);

  // 3. Recognize
  // We reset mock to 0, but recognize_loop will flip it halfway
  DEBUG_OUT_STR("\n[Mode] Recognition...\n");
  mock_scenario = 0;

  begin_event();
  recognize_loop(&global_model);
#ifdef DEBUG
  delay(SEC_TO_CYCLES);
#endif
  end_event();

  end_measurement_window();

  return 0;
}