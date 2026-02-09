/* D1. VM global restored at boundary.
 * Global in VM, live-in at region boundary. Blocks large enough to
 * force a boundary.
 * Expected: __restore_mem call at boundary in the instrumented IR. */

int g_vol;
volatile int barrier;

int litmus_needvol(int x) {
    /* Write VM global in entry block, pad with instructions */
    g_vol = x + 99;
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
    int k = j + 11;
    int l = k + 12;

    if (barrier) {
        /* Read the VM global after boundary (needs volatile restore) */
        int m = l + g_vol;
        int n = m + 1;
        int o = n + 2;
        int p = o + 3;
        int q = p + 4;
        int r = q + 5;
        int s = r + 6;
        int t = s + 7;
        int u = t + 8;
        int v = u + 9;
        int w = v + 10;
        return w;
    }

    return l;
}
