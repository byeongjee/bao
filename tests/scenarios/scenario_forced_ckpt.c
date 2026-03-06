/* A2. Energy overflow forces a boundary.
 * Two phases separated by a volatile-guarded branch. Each phase has
 * ~15 instructions, so the path (30+ energy) exceeds capacity (25).
 * Expected: Region starts (2+). */

volatile int barrier;

int main(void) {
    int a = 1;
    /* Phase 1: ~14 adds → ~17 energy with load+icmp+br */
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
    int l = k + 11;
    int m = l + 12;
    int n = m + 13;
    int o = n + 14;

    if (barrier) {
        /* Phase 2: ~14 adds → ~15 energy */
        int p = o + 15;
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
        int aa = z + 26;
        int bb = aa + 27;
        int cc = bb + 28;
        return cc;
    }

    return o;
}
