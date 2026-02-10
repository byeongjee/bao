/* C2. Too many globals for VM.
 * 5 globals x 4 bytes = 20 > vm_capacity=16.
 * Blocks large enough to force a boundary.
 * Expected: VM-placed globals: 4/5; one global gets .nvm section. */

int g_a, g_b, g_c, g_d, g_e;
volatile int barrier;

int main(void) {
    int x = 1;
    /* Write all 5 globals, pad with enough instructions */
    g_a = x + 1;
    g_b = x + 2;
    g_c = x + 3;
    g_d = x + 4;
    g_e = x + 5;
    int a = x + 6;
    int b = a + 7;
    int c = b + 8;
    int d = c + 9;
    int e = d + 10;
    int f = e + 11;
    int g = f + 12;

    if (barrier) {
        /* Use all globals after boundary */
        int h = g + g_a;
        int i = h + g_b;
        int j = i + g_c;
        int k = j + g_d;
        int l = k + g_e;
        int m = l + 1;
        int n = m + 2;
        int o = n + 3;
        int p = o + 4;
        int q = p + 5;
        g_a = q;
    }
    return 0;
}
