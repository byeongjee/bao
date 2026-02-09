/* A1. No checkpoint needed — total energy < capacity (25).
 * A short function with only a few instructions.
 * Expected: Region starts (1) — entry only. */

int scenario_no_ckpt(int a, int b) {
    int c = a + b;
    int d = c - 1;
    return d;
}
