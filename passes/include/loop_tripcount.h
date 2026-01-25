#ifndef LOOP_TRIPCOUNT_H
#define LOOP_TRIPCOUNT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Annotate the containing loop with a maximum trip count.
 * Place as the FIRST statement inside the loop body.
 *
 * Example:
 *   for (int i = 0; i < n; i++) {
 *       __loop_tripcount(100);
 *       // ... loop body
 *   }
 *
 * WARNING: This marker may inhibit certain compiler optimizations
 * (vectorization, unrolling, LICM). This annotation is intended for
 * static analysis purposes only. For production builds, consider:
 *   1. Running analysis on unoptimized IR (-O0)
 *   2. Stripping marker calls before optimization passes
 *   3. Using a separate analysis build configuration
 */
extern void __loop_tripcount(int max_iterations) __attribute__((noinline));

#ifdef __cplusplus
}
#endif

#endif // LOOP_TRIPCOUNT_H
