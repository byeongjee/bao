#ifndef LOOP_TRIPCOUNT_H
#define LOOP_TRIPCOUNT_H

/**
 * Annotate the containing loop with a maximum trip count.
 * Place as the FIRST statement inside the loop body.
 *
 * This stub allows standalone compilation without warnings.
 * The MILP/SCHEMATIC passes detect calls to __loop_tripcount
 * in the unoptimized IR regardless of linkage.
 */
static inline void __loop_tripcount(int max_iterations)
{
    (void)max_iterations;
}

#endif /* LOOP_TRIPCOUNT_H */
