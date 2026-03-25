/*
 * SCHEMATIC Mock Checkpoint Counter Runtime
 *
 * Implements __region_boundary as a counter stub for host-based evaluation.
 * cnt_store_mem and cnt_restore_mem are globals incremented inline at IR level
 * by the instrumenter (load-add-store pattern).
 *
 * Call __schematic_print_counts() at program exit to print a summary.
 *
 * Intended for evaluation under continuous power only.
 */

#include <stdint.h>
#include <stdio.h>

#include "benchmark.h"

static uint32_t cnt_boundary;
static uint32_t cnt_save_reg;
static uint32_t cnt_restore_reg;

/* These are incremented inline by the instrumenter at IR level.
   Declared as globals (not static) so the instrumenter can reference them. */
uint32_t cnt_store_mem;
uint32_t cnt_restore_mem;

void __region_boundary(void) {
    cnt_boundary++;
    cnt_save_reg += 12;    /* Simulate bulk save of R4-R15 */
    cnt_restore_reg += 12; /* Simulate bulk restore of R4-R15 */
}

/* No-op stub for loop trip-count annotations that survive into the final IR. */
void __loop_tripcount(int max_iterations) {
    (void)max_iterations;
}

__attribute__((destructor)) void __schematic_print_counts(void) {
    printf("=== Debug Counter Summary ===\n");
    printf("  __region_boundary:    %u\n", cnt_boundary);
    printf("  reg_saves:            %u\n", cnt_save_reg);
    printf("  reg_restores:         %u\n", cnt_restore_reg);
    printf("  mem_stores:           %u\n", (uint32_t)cnt_store_mem);
    printf("  mem_restores:         %u\n", (uint32_t)cnt_restore_mem);
    printf(DEBUG_END_MARKER "\n");
}
