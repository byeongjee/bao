/* B2. Global memory store at def site.
 * Global written before a volatile-guarded branch, read after boundary.
 * Blocks large enough to force a boundary.
 * Expected: __checkpoint_store_mem call in the instrumented IR. */

int g_data;
volatile int barrier;

void litmus_store_global(int x) {
    /* Write global in entry block, pad with enough instructions */
    g_data = x + 42;
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
        /* Read global after boundary */
        int m = l + g_data;
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
        g_data = w;
    }
}
