/* NVM efficiency test: g_rare accessed in 2 blocks but live-in at 3+
 * boundaries. Store cost is paid once (at def site). Restore cost
 * depends on how many boundaries have needs_vol_restore.
 * Expected: g_rare placed in VM (store once, restore cost amortized). */

int g_rare __attribute__((annotate("milp_candidate")));
volatile int barrier;

int main(void) {
    int x = 1;
    /* Phase 1: write g_rare once, pad entry block */
    g_rare = x + 42;
    int a = x + 1;
    int b = a + 2;
    int c = b + 3;
    int d = c + 4;
    int e = d + 5;
    int f = e + 6;
    int g = f + 7;
    int h = g + 8;
    int i = h + 9;
    int j = i + 10;

    if (barrier) {
        /* Phase 2: g_rare NOT accessed, just computation */
        int k = j + 11;
        int l = k + 12;
        int m = l + 13;
        int n = m + 14;
        int o = n + 15;
        int p = o + 16;
        int q = p + 17;
        int r = q + 18;
        int s = r + 19;
        int t = s + 20;

        if (barrier) {
            /* Phase 3: g_rare NOT accessed, just computation */
            int u = t + 21;
            int v = u + 22;
            int w = v + 23;
            int y = w + 24;
            int z = y + 25;
            int aa = z + 26;
            int bb = aa + 27;
            int cc = bb + 28;
            int dd = cc + 29;
            int ee = dd + 30;

            if (barrier) {
                /* Phase 4: read g_rare once */
                int ff = ee + g_rare;
                return ff;
            }
            return ee;
        }
        return t;
    }
    return j;
}
