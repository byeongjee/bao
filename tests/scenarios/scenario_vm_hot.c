/* C1. Hot global placed in VM.
 * Global accessed many times in a loop — should be placed in VM (SRAM)
 * to avoid NVM access penalty.
 * Expected: VM-placed globals: 1/1. */

int g_hot __attribute__((annotate("milp_candidate")));

int main(void) {
    int n = 10;
    g_hot = 0;
    for (int i = 0; i < n; i++) {
        g_hot += i;
    }
    return g_hot;
}
