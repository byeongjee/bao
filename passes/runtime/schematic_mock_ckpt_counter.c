/*
 * SCHEMATIC Mock Checkpoint Counter Runtime
 *
 * Implements the 6 SCHEMATIC checkpoint functions as no-op counters.
 * Each function increments a static counter without performing any NVM
 * operations.  Call __schematic_print_counts() at program exit to print a
 * summary of all counters.
 *
 * Intended for evaluation under continuous power only.
 */

#include <stdint.h>
#include <stdio.h>

#include "debug_counters.h"

static uint32_t cnt_region_prologue;
static uint32_t cnt_region_epilogue;
static uint32_t cnt_checkpoint_store_reg;
static uint32_t cnt_checkpoint_store_mem;
static uint32_t cnt_restore_reg;
static uint32_t cnt_restore_mem;

void __region_prologue(void) {
    cnt_region_prologue++;
}

void __region_epilogue(void) {
    cnt_region_epilogue++;
}

void __checkpoint_store_reg(int32_t slot_id, int64_t value) {
    (void)slot_id;
    (void)value;
    cnt_checkpoint_store_reg++;
}

void __checkpoint_store_mem(void *nvm_dst, void *vm_src, int32_t size) {
    (void)nvm_dst;
    (void)vm_src;
    (void)size;
    cnt_checkpoint_store_mem++;
}

void __restore_reg(int32_t slot_id, void *dest) {
    (void)slot_id;
    (void)dest;
    cnt_restore_reg++;
}

void __restore_mem(void *vm_dst, void *nvm_src, int32_t size) {
    (void)vm_dst;
    (void)nvm_src;
    (void)size;
    cnt_restore_mem++;
}

/* No-op stub for loop trip-count annotations that survive into the final IR. */
void __loop_tripcount(int max_iterations) {
    (void)max_iterations;
}

__attribute__((destructor)) void __schematic_print_counts(void) {
    printf("=== SCHEMATIC Checkpoint Counter Summary ===\n");
    printf("  __region_prologue:       %u\n", cnt_region_prologue);
    printf("  __region_epilogue:       %u\n", cnt_region_epilogue);
    printf("  __checkpoint_store_reg:  %u\n", cnt_checkpoint_store_reg);
    printf("  __checkpoint_store_mem:  %u\n", cnt_checkpoint_store_mem);
    printf("  __restore_reg:           %u\n", cnt_restore_reg);
    printf("  __restore_mem:           %u\n", cnt_restore_mem);
    printf(DEBUG_END_MARKER "\n");
}
