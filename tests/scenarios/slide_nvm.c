/* Slide: NVM placement for global variables.
 * Two globals but VM (SRAM) only fits one (vm_capacity=4 bytes).
 * g_hot is read/written in a loop → high access frequency → placed in VM.
 * g_cold is accessed once → low frequency → placed in NVM.
 * Expected: g_hot in VM, g_cold in NVM (.nvm section). */

int g_hot __attribute__((annotate("milp_candidate")));
int g_cold __attribute__((annotate("milp_candidate")));
volatile int barrier;

int main(void) {
    int x = 1;
    g_hot = 0;
    g_cold = x + 42;
    /* Padding to force checkpoint boundary */
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
        /* g_hot accessed in a loop (frequency = 10) */
        for (int k = 0; k < 10; k++) {
            g_hot += k;
        }
        /* g_cold accessed once (frequency = 1) */
        return g_hot + g_cold;
    }

    return j;
}
