/* Pointer-induction-variable access to a VM-placed global array.
 * The loop walks g_hot through a pointer phi, so every access reaches the
 * global only via the phi — invisible to plain getUnderlyingObject.
 * Expected: the pass does not skip the function (strict mode resolves the
 * single-base phi), g_hot gets a shadow, and the phi-based accesses are
 * rewritten to shadow + (p - g_hot). */

int g_hot[4];
int g_cold;

int main(void) {
    for (int *p = g_hot; p != g_hot + 4; ++p)
        *p = *p + 1;
    g_cold = g_hot[0];
    return g_cold;
}
