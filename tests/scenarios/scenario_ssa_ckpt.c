/* Cross-block SSA register as V_inelig gets MILP-optimized commit/restore.
 * g_candidate is annotated (V_elig), x is an SSA value used across boundary.
 * Phase 2 uses x (defined in Phase 1), making it live-in at the boundary.
 * Expected: @__nvm_ssa_N in .nvm; typed store/load for commit/restore;
 * uses of %x in Phase 2 replaced with restored value. */

int g_candidate __attribute__((annotate("milp_candidate")));
volatile int barrier;

int main(void) {
    /* Phase 1: define SSA value x from candidate */
    g_candidate = 1;
    int x = g_candidate + 10;
    int a = x + 1;
    int b = a + 2;
    int c = b + 3;
    int d = c + 4;
    int e = d + 5;
    int f = e + 6;

    if (barrier) {
        /* Phase 2: USE x (live-in SSA register) */
        g_candidate = x + 5;
        int h = g_candidate + x;
        int i = h + 9;
        int j = i + 10;
        int k = j + 11;
        int m = k + 12;
        int n = m + 13;
        return n;
    }

    return x;
}
