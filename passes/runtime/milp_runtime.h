/*
 * MILP Checkpoint Runtime (MSP430)
 *
 * Declares the 6 runtime functions called by the MILP checkpoint insertion
 * pass (CheckpointInstrumenter):
 *
 *   __region_prologue / __region_epilogue   – region boundary markers
 *   __checkpoint_store_reg / __restore_reg  – register save/restore
 *   __checkpoint_store_mem / __restore_mem  – memory-range save/restore
 */

#ifndef MILP_RUNTIME_H
#define MILP_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void __region_prologue(void);
void __region_epilogue(void);
void __checkpoint_store_reg(int32_t slot_id, int64_t value);
void __checkpoint_store_mem(void *nvm_dst, void *vm_src, int32_t size);
void __restore_reg(int32_t slot_id, void *dest);
void __restore_mem(void *vm_dst, void *nvm_src, int32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MILP_RUNTIME_H */
