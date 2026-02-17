/* Non-candidate globals get MILP-optimized commit/restore.
 * g_candidate is annotated (V_elig), g_noncandidate is not (V_inelig).
 * Two phases separated by a volatile-guarded branch force a boundary.
 * Expected: candidate gets .nvm + shadow; non-candidate stays in SRAM
 * with __nvm_backup_ in .nvm; commit/restore emitted for both at boundary. */

int g_candidate __attribute__((annotate("milp_candidate")));
int g_noncandidate;
volatile int barrier;

int main(void) {
    /* Phase 1: write both globals, use enough instructions to approach capacity */
    g_candidate = 1;
    g_noncandidate = 2;
    int a = g_candidate + g_noncandidate;
    int b = a + 1;
    int c = b + 2;
    int d = c + 3;
    int e = d + 4;
    int f = e + 5;
    int g = f + 6;
    int h = g + 7;
    int i = h + 8;
    int j = i + 9;
    int k = j + 10;
    int m = k + 11;
    int n = m + 12;

    if (barrier) {
        /* Phase 2: read/write both globals again */
        g_candidate = n + 13;
        g_noncandidate = g_candidate + 14;
        int p = g_noncandidate + 15;
        int q = p + 16;
        int r = q + 17;
        int s = r + 18;
        int t = s + 19;
        int u = t + 20;
        int v = u + 21;
        int w = v + 22;
        int x = w + 23;
        int y = x + 24;
        int z = y + 25;
        return z;
    }

    return n;
}
