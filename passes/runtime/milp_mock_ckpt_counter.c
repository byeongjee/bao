/*
 * MILP Mock Checkpoint Counter Runtime
 *
 * Implements __region_boundary as a no-op counter and provides globals
 * for IR-level debug counters (cnt_save_vreg, cnt_restore_vreg, etc.)
 * that are incremented directly by the MILP instrumenter.
 *
 * Call __milp_print_counts() at program exit to print a summary.
 *
 * Intended for evaluation under continuous power only.
 */

#include <stdint.h>
#include <stdio.h>

#include "benchmark.h"

static uint32_t cnt_region_boundary;

/* IR-level debug counters — incremented by load-add-store sequences
   inserted by CheckpointInstrumenter when --add-debug-markers is used.
   Must be non-static so the LLVM pass can resolve them. */
uint32_t cnt_save_vreg;
uint32_t cnt_restore_vreg;
uint32_t cnt_store_mem;
uint32_t cnt_restore_mem;

void __region_boundary(void) {
    cnt_region_boundary++;
}

/* No-op stub for loop trip-count annotations that survive into the final IR. */
void __loop_tripcount(int max_iterations) {
    (void)max_iterations;
}

__attribute__((destructor)) void __milp_print_counts(void) {
    printf("=== MILP Checkpoint Counter Summary ===\n");
    printf("  __region_boundary:    %u\n", cnt_region_boundary);
    printf("  vreg_saves:           %u\n", cnt_save_vreg);
    printf("  vreg_restores:        %u\n", cnt_restore_vreg);
    printf("  mem_stores:           %u\n", cnt_store_mem);
    printf("  mem_restores:         %u\n", cnt_restore_mem);
    printf(DEBUG_END_MARKER "\n");
}
