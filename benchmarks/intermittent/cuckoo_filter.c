#include <stdbool.h>
#include <stdint.h>

#define FORCE_INLINE static inline __attribute__((always_inline))

#define NUM_BUCKETS 256 // must be a power of 2
#define MAX_RELOCATIONS 8

typedef uint16_t value_t;
typedef uint16_t hash_t;
typedef uint16_t fingerprint_t;
typedef uint16_t index_t; // bucket index

// We aim to fill 50% of the buckets for this demo
#define NUM_KEYS (NUM_BUCKETS / 2)
#define INIT_KEY 0x1

// Storage for the filter (preserved with attribute)
static fingerprint_t filter[NUM_BUCKETS] __attribute__((used));

// LFSR state for random number generation
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

// --- Core Cuckoo Logic ---

FORCE_INLINE hash_t djb_hash(uint8_t *data, unsigned len) {
  uint32_t hash = 5381;
  unsigned int i;
  for (i = 0; i < len; data++, i++)
    hash = ((hash << 5) + hash) + (*data);
  return hash & 0xFFFF;
}

// Map a fingerprint to an index
FORCE_INLINE index_t hash_fp_to_index(fingerprint_t fp) {
  hash_t hash = djb_hash((uint8_t *)&fp, sizeof(fingerprint_t));
  return hash & (NUM_BUCKETS - 1);
}

// Map a key (original value) to an index
FORCE_INLINE index_t hash_key_to_index(value_t key) {
  hash_t hash = djb_hash((uint8_t *)&key, sizeof(value_t));
  return hash & (NUM_BUCKETS - 1);
}

// Generate the fingerprint (short hash) for a key
FORCE_INLINE fingerprint_t hash_to_fingerprint(value_t key) {
  fingerprint_t fp = djb_hash((uint8_t *)&key, sizeof(value_t));
  // Fingerprint cannot be 0 (0 denotes empty slot)
  return (fp == 0) ? 1 : fp;
}

// Deterministic key generator for testing
FORCE_INLINE value_t generate_key(value_t prev_key) { return (prev_key + 1) * 17; }

FORCE_INLINE bool insert(fingerprint_t *filter, value_t key) {
  fingerprint_t fp1, fp2, fp_victim, fp_next_victim;
  index_t index_victim, fp_hash_victim;
  unsigned relocation_count = 0;

  fingerprint_t fp = hash_to_fingerprint(key);
  index_t index1 = hash_key_to_index(key);
  index_t fp_hash = hash_fp_to_index(fp);

  // Cuckoo Filter Property: Index2 = Index1 XOR Hash(fingerprint)
  index_t index2 = index1 ^ fp_hash;
  // Mask again just to be safe against overflow
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
    return false;
  }

  return true;
}

FORCE_INLINE bool lookup(fingerprint_t *filter, value_t key) {
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

// --- Main ---

__attribute__((noinline)) int main() {
  lfsr_state = 0xACE1u;

  // Clear Filter
  for (int i = 0; i < NUM_BUCKETS; i++)
    filter[i] = 0;

  value_t key = INIT_KEY;
  volatile unsigned inserts = 0;

  // 1. Insertion Phase
  for (int i = 0; i < NUM_KEYS; ++i) {
    key = generate_key(key);
    bool success = insert(filter, key);

    if (success)
      inserts++;
  }

  // 2. Verification Phase
  key = INIT_KEY; // Reset key generator
  volatile unsigned found = 0;

  for (int i = 0; i < NUM_KEYS; ++i) {
    key = generate_key(key);
    bool member = lookup(filter, key);

    if (member)
      found++;
  }

  return (int)(inserts + found);
}
