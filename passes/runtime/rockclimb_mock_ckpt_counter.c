/*
 * RockClimb Mock Checkpoint Counter Runtime
 *
 * Implements the 5 RockClimb runtime functions as no-op counters.
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
static uint32_t cnt_init;
static uint32_t cnt_is_recovery;
static uint32_t cnt_recover;

void __rockclimb_check(void) {
    cnt_check++;
}

void __rockclimb_save_reg(uint8_t reg_id, uint16_t value) {
    (void)reg_id;
    (void)value;
    cnt_save_reg++;
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

void __rockclimb_print_counts(void) {
    printf("=== RockClimb Checkpoint Counter Summary ===\n");
    printf("  __rockclimb_check:        %u\n", cnt_check);
    printf("  __rockclimb_save_reg:     %u\n", cnt_save_reg);
    printf("  __rockclimb_init:         %u\n", cnt_init);
    printf("  __rockclimb_is_recovery:  %u\n", cnt_is_recovery);
    printf("  __rockclimb_recover:      %u\n", cnt_recover);
    printf("=============================================\n");
}
