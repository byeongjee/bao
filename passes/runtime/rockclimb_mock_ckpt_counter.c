/*
 * RockClimb Mock Checkpoint Counter Runtime
 *
 * Implements RockClimb runtime functions and optional debug markers
 * as no-op counters.
 * Each function increments a static counter without performing any NVM
 * operations.  Call __rockclimb_print_counts() at program exit to print a
 * summary of all counters.
 *
 * Intended for evaluation under continuous power only.
 */

#include <stdint.h>
#include <stdio.h>

#include "rockclimb_runtime.h"

/* NVM variable definitions (satisfy extern declarations in header) */
uint16_t __nvm_regs[ROCKCLIMB_MAX_REGS];
uint16_t __nvm_region_id;
uint16_t __nvm_pc;
uint16_t __nvm_sp;
volatile uint16_t __rockclimb_vmax_threshold;

static uint32_t cnt_check;
static uint32_t cnt_save_reg;
static uint32_t cnt_region_prologue;
static uint32_t cnt_region_epilogue;
static uint32_t cnt_checkpoint_store_reg;
static uint32_t cnt_init;
static uint32_t cnt_is_recovery;
static uint32_t cnt_recover;

void __rockclimb_check(void) {
    cnt_check++;
}

void __rockclimb_save_reg(void) {
    cnt_save_reg++;
}

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

/* No-op stub for loop trip-count annotations that survive into final IR. */
void __loop_tripcount(int max_iterations) {
    (void)max_iterations;
}

void __rockclimb_init(void) {
    cnt_init++;
}

uint16_t __rockclimb_is_recovery(void) {
    cnt_is_recovery++;
    return 0;
}

void __rockclimb_recover(void) {
    cnt_recover++;
}

__attribute__((destructor)) void __rockclimb_print_counts(void) {
    printf("=== RockClimb Checkpoint Counter Summary ===\n");
    printf("  __rockclimb_check:        %u\n", cnt_check);
    printf("  __rockclimb_save_reg:     %u\n", cnt_save_reg);
    printf("  __region_prologue:        %u\n", cnt_region_prologue);
    printf("  __region_epilogue:        %u\n", cnt_region_epilogue);
    printf("  __checkpoint_store_reg:   %u\n", cnt_checkpoint_store_reg);
    printf("  __rockclimb_init:         %u\n", cnt_init);
    printf("  __rockclimb_is_recovery:  %u\n", cnt_is_recovery);
    printf("  __rockclimb_recover:      %u\n", cnt_recover);
    printf("=============================================\n");
}
