/* Test: merge-point VM placement divergence at do-while loop header.
 *
 * g_counter is a writable global written ONCE in the preheader (entry)
 * and heavily accessed in the loop body.  A do-while loop compiles to
 * a single block whose header IS the only block — any region boundary
 * must be at this merge point (preds: entry + backedge).
 *
 * The optimizer prefers placeInVm[entry]=0 (one NVM access is cheaper
 * than restore) but placeInVm[loop]=1 (many accesses benefit from VM).
 * Without the merge-point consistency constraint, this creates divergent
 * placeInVm at the loop header boundary.
 *
 * Expected: the MILP constraint prevents this divergence.
 */

int g_counter;

void __loop_tripcount(int);

int main(void) {
    /* Preheader: one write to g_counter */
    g_counter = 100;

    /* do-while: single body block = merge point */
    int i = 0;
    do {
        __loop_tripcount(20);
        g_counter += i;
        g_counter += 1;
        i++;
    } while (i < 20);

    return g_counter;
}
