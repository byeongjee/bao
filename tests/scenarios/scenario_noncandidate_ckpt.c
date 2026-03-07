/* All accessed globals are MILP candidates (V_elig).
 * Both g_candidate and g_noncandidate are directly accessed, so both
 * are eligible for VM/NVM placement optimization.
 * Two phases separated by a volatile-guarded branch force a boundary.
 * Phase 2 READS both globals before writing, making them live-in
 * at the boundary so that commit/restore are actually emitted. */

int g_candidate;
int g_noncandidate;
volatile int barrier;

int main(void) {
    /* Phase 1: write both globals, enough instructions to fill a region */
    g_candidate = 1;
    g_noncandidate = 2;
    int a = g_candidate + g_noncandidate;
    int b = a + 1;
    int c = b + 2;
    int d = c + 3;
    int e = d + 4;
    int f = e + 5;
    int g = f + 6;

    if (barrier) {
        /* Phase 2: READ both globals first (live-in), then write */
        int h = g_candidate + g_noncandidate;
        g_candidate = h + 7;
        g_noncandidate = h + 8;
        int i = h + 9;
        int j = i + 10;
        int k = j + 11;
        int m = k + 12;
        int n = m + 13;
        return n;
    }

    return g;
}
