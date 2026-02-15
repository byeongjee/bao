/* Slide: distributed checkpoint stores.
 * 'result' is defined in the entry block and used after the checkpoint
 * boundary. The optimizer inserts __checkpoint_store_reg at the
 * definition site to persist 'result' across a power failure.
 * Expected: __checkpoint_store_reg call for 'result' in entry block. */

volatile int barrier;

int main(void) {
    int x = 1;
    int result = x * 3 + 7;  /* <-- defined here, saved to NVM here */
    /* Padding to force energy overflow → checkpoint boundary */
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
        /* 'result' is live-in here → needs restore after reboot */
        int k = j + result;  /* <-- used here, across checkpoint */
        int l = k + 1;
        int m = l + 2;
        int n = m + 3;
        int o = n + 4;
        int p = o + 5;
        int q = p + 6;
        int r = q + 7;
        int s = r + 8;
        int t = s + 9;
        return t;
    }

    return j + result;
}
