/* C1. Hot global placed in VM.
 * Global accessed many times in a loop — should be placed in VM (SRAM)
 * to avoid NVM access penalty.
 * Expected: VM-placed globals: 1/1. */

int g_hot;

int litmus_vm_hot(int n) {
    g_hot = 0;
    for (int i = 0; i < n; i++) {
        g_hot += i;
    }
    return g_hot;
}
