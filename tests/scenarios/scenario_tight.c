/* E1. Very tight capacity (uses scenario_tight_config.json with capacity=12).
 * Chain of volatile-guarded branches, each block ~8 instructions.
 * Expected: Region starts (3+). */

volatile int barrier;

int main(void) {
    int x = 1;
    int a = x + 1;
    int b = a + 2;
    int c = b + 3;
    int d = c + 4;
    int e = d + 5;
    int f = e + 6;

    if (barrier) {
        int g = f + 7;
        int h = g + 8;
        int i = h + 9;
        int j = i + 10;
        int k = j + 11;
        int l = k + 12;

        if (barrier) {
            int m = l + 13;
            int n = m + 14;
            int o = n + 15;
            int p = o + 16;
            int q = p + 17;
            int r = q + 18;

            if (barrier) {
                int s = r + 19;
                int t = s + 20;
                int u = t + 21;
                int v = u + 22;
                int w = v + 23;
                int y = w + 24;
                return y;
            }
            return r;
        }
        return l;
    }
    return f;
}
