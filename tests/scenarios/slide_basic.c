/* Slide: basic checkpoint placement.
 * Two computation phases whose combined energy exceeds capacity (25).
 * The MILP places a checkpoint between the phases.
 * Expected: 1 checkpoint at the start of Phase 2. */

volatile int barrier;

int main(void) {
    int x = 1;
    /* Phase 1 (~15 energy after mem2reg: 12 adds + load + icmp + br) */
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
        /* Phase 2 (~13 energy: 12 adds + ret) */
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
        int y = w + 24;
        return y;
    }

    return l;
}
