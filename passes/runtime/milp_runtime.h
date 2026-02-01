/*
 * MILP Checkpoint Runtime (MSP430)
 *
 * This is a placeholder runtime for the MILP-based checkpoint insertion pass.
 * The LLVM pass inserts calls to a checkpoint function with the signature:
 *
 *   void fn(const char* block_name)
 *
 * For MILP mode, we use `__milp_checkpoint` as the runtime entrypoint.
 */

#ifndef MILP_RUNTIME_H
#define MILP_RUNTIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MILP entrypoint used by --mode milp builds. */
void __milp_checkpoint(const char *block_name);

#ifdef __cplusplus
}
#endif

#endif /* MILP_RUNTIME_H */
