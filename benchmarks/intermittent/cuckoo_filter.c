#include "setup.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define NUM_BUCKETS 256 // must be a power of 2
#define MAX_RELOCATIONS 8

typedef uint16_t value_t;
typedef uint16_t hash_t;
typedef uint16_t fingerprint_t;
typedef uint16_t index_t; // bucket index

// --- Simulation Configuration ---

// We aim to fill 50% of the buckets for this demo
#define NUM_KEYS (NUM_BUCKETS / 2)
#define INIT_KEY 0x1

// LED Config for FR5994 LaunchPad
#define LED1_PIN BIT0 // P1.0 (Red)
#define LED2_PIN BIT1 // P1.1 (Green)

// Storage for the filter (Zero initialized by startup code or explicit loop)
static fingerprint_t filter[NUM_BUCKETS];

// this should be initialized to a random value
// we don't do it here because currently my interpreter
// doesn't support initializing variables from data section
static uint16_t lfsr_state;

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

// --- Core Cuckoo Logic ---

INLINE hash_t djb_hash(uint8_t *data, unsigned len) {
  uint32_t hash = 5381;
  unsigned int i;
  for (i = 0; i < len; data++, i++)
    hash = ((hash << 5) + hash) + (*data);
  return hash & 0xFFFF;
}

// Map a fingerprint to an index
INLINE index_t hash_fp_to_index(fingerprint_t fp) {
  hash_t hash = djb_hash((uint8_t *)&fp, sizeof(fingerprint_t));
  return hash & (NUM_BUCKETS - 1);
}

// Map a key (original value) to an index
INLINE index_t hash_key_to_index(value_t key) {
  hash_t hash = djb_hash((uint8_t *)&key, sizeof(value_t));
  return hash & (NUM_BUCKETS - 1);
}

// Generate the fingerprint (short hash) for a key
INLINE fingerprint_t hash_to_fingerprint(value_t key) {
  fingerprint_t fp = djb_hash((uint8_t *)&key, sizeof(value_t));
  // Fingerprint cannot be 0 (0 denotes empty slot)
  return (fp == 0) ? 1 : fp;
}

// Deterministic key generator for testing
INLINE value_t generate_key(value_t prev_key) { return (prev_key + 1) * 17; }

INLINE bool insert(fingerprint_t *filter, value_t key) {
  fingerprint_t fp1, fp2, fp_victim, fp_next_victim;
  index_t index_victim, fp_hash_victim;
  unsigned relocation_count = 0;

  fingerprint_t fp = hash_to_fingerprint(key);
  index_t index1 = hash_key_to_index(key);
  index_t fp_hash = hash_fp_to_index(fp);

  // Cuckoo Filter Property: Index2 = Index1 XOR Hash(fingerprint)
  index_t index2 = index1 ^ fp_hash;
  // Mask again just to be safe against overflow (though XOR shouldn't overflow
  // power of 2 bounds)
  index2 &= (NUM_BUCKETS - 1);

  fp1 = filter[index1];
  if (fp1 == 0) { // Slot 1 free
    filter[index1] = fp;
    return true;
  }

  fp2 = filter[index2];
  if (fp2 == 0) { // Slot 2 free
    filter[index2] = fp;
    return true;
  }

  // Both slots full. Evict a victim.
  // Randomly choose index1 or index2 to start the kicking chain
  index_victim = (simple_rand() & 0x80) ? index1 : index2;
  fp_victim = filter[index_victim];
  filter[index_victim] = fp; // Place new item, holding victim in hand

  // Relocation Loop (The "Cuckoo" part)
  do {
    // Calculate the "other" address for the victim
    fp_hash_victim = hash_fp_to_index(fp_victim);
    index_victim = index_victim ^ fp_hash_victim;
    index_victim &= (NUM_BUCKETS - 1);

    fp_next_victim = filter[index_victim];
    filter[index_victim] = fp_victim;

    // If the slot we moved into was not empty, we pick up the next victim
    fp_victim = fp_next_victim;
    relocation_count++;

  } while (fp_victim != 0 && relocation_count < MAX_RELOCATIONS);

  if (fp_victim != 0) {
    DEBUG_OUT_STR("FAILED: Max relocations (");
    DEBUG_OUT_U16(MAX_RELOCATIONS);
    DEBUG_OUT_STR(") reached. Dropped FP: ");
    DEBUG_OUT_HEX(fp_victim);
    DEBUG_OUT_CHAR('\n');
    return false;
  }

  return true;
}

INLINE bool lookup(fingerprint_t *filter, value_t key) {
  fingerprint_t fp = hash_to_fingerprint(key);
  index_t index1 = hash_key_to_index(key);
  index_t fp_hash = hash_fp_to_index(fp);
  index_t index2 = (index1 ^ fp_hash) & (NUM_BUCKETS - 1);

  if (filter[index1] == fp)
    return true;
  if (filter[index2] == fp)
    return true;
  return false;
}

// --- Visualization ---

void print_filter(fingerprint_t *f) {
  DEBUG_OUT_STR("\n--- Filter State (Partial View) ---\n");
  int occupied = 0;
  // Only printing first 64 buckets to save UART time, or all if you prefer
  for (int i = 0; i < NUM_BUCKETS; i++) {
    if (f[i] != 0) {
      occupied++;
      if (i < 32) { // Just print first 32 non-empty slots to keep log clean
        DEBUG_OUT_STR("[");
        DEBUG_OUT_U16(i);
        DEBUG_OUT_STR("]: ");
        DEBUG_OUT_HEX(f[i]);
        DEBUG_OUT_STR("  ");
        if (occupied % 4 == 0) {
          DEBUG_OUT_CHAR('\n');
        }
      }
    }
  }
  DEBUG_OUT_STR("\nTotal Occupied: ");
  DEBUG_OUT_U16(occupied);
  DEBUG_OUT_STR(" / ");
  DEBUG_OUT_U16(NUM_BUCKETS);
  DEBUG_OUT_CHAR('\n');
  DEBUG_OUT_STR("--------------------\n");
}

// --- Main ---

int main() {
  initialize();

  __enable_interrupt();

  lfsr_state = 0xACE1u;

  // Clear Filter
  for (int i = 0; i < NUM_BUCKETS; i++)
    filter[i] = 0;

  DEBUG_OUT_STR("\n\n=== Cuckoo Filter Demo (Updated) ===\n");
  DEBUG_OUT_STR("Buckets: ");
  DEBUG_OUT_U16(NUM_BUCKETS);
  DEBUG_OUT_STR(", Relocation Limit: ");
  DEBUG_OUT_U16(MAX_RELOCATIONS);
  DEBUG_OUT_CHAR('\n');
  DEBUG_OUT_STR("Attempting to insert ");
  DEBUG_OUT_U16(NUM_KEYS);
  DEBUG_OUT_STR(" keys...\n");

  value_t key = INIT_KEY;
  unsigned inserts = 0;

  begin_measurement_window();

  // 1. Insertion Phase
  DEBUG_OUT_STR("\n[Phase 1] Inserting...\n");
  begin_event();
  for (int i = 0; i < NUM_KEYS; ++i) {
    key = generate_key(key);
    bool success = insert(filter, key);

    if (success)
      inserts++;

    // Blink Red LED on success
    DEBUG_OUT_U16(i);
#ifdef DEBUG
    P1OUT ^= LED1_PIN;
    delay(5000);
#endif
  }
  end_event();

#ifdef DEBUG
  print_filter(filter);
#endif
  DEBUG_OUT_STR("Insert Success Rate: ");
  DEBUG_OUT_U16(inserts);
  DEBUG_OUT_STR(" / ");
  DEBUG_OUT_U16(NUM_KEYS);
  DEBUG_OUT_CHAR('\n');

  // 2. Verification Phase
  DEBUG_OUT_STR("\n[Phase 2] Verifying...\n");
  key = INIT_KEY;              // Reset key generator
  volatile unsigned found = 0; // volatile to prevent optimization

  begin_event();
  for (int i = 0; i < NUM_KEYS; ++i) {
    key = generate_key(key);
    bool member = lookup(filter, key);

    if (member)
      found++;
    else {
      // This is expected if 'insert' failed earlier for this key
      // printf("Key %04x missing (likely dropped during insert)\n", key);
    }

// Blink Green LED on check
#ifdef DEBUG
    P1OUT ^= LED2_PIN;
    delay(5000);
#endif
  }
  end_event();

  end_measurement_window();

  DEBUG_OUT_STR("Lookup Success Rate: ");
  DEBUG_OUT_U16(found);
  DEBUG_OUT_STR(" / ");
  DEBUG_OUT_U16(NUM_KEYS);
  DEBUG_OUT_CHAR('\n');
  DEBUG_OUT_STR("Demo Complete.\n");

  return 0;
}