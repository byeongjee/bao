/* B1. SSA register store at def site.
 * Value 'val' defined in entry block, live across a boundary into a
 * later block. Blocks are large enough to force a boundary.
 * Expected: Enabled checkpoint stores > 0; __checkpoint_store_reg calls
 * appear in the instrumented IR. */

volatile int barrier;

int main(void) {
    int x = 1;
    /* Define 'val' early, then pad to force boundary */
    int val = x * 3 + 7;
    int a = val + 1;
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
        /* 'val' is live-in here across the boundary */
        int m = l + 13;
        int n = m + 14;
        int o = n + 15;
        int p = o + 16;
        int q = p + 17;
        int r = q + 18;
        int s = r + 19;
        int t = s + 20;
        int u = t + 21;
        int v = u + 22;
        int w = v + 23;
        int y = w + val;
        return y;
    }

    return l + val;
}
