/* C3. VM/NVM enforcement: shadow globals and access rewriting.
 * Two globals: one hot (many accesses), one cold (one access).
 * Expected: hot global gets shadow + rewritten accesses; both get .nvm section.
 * Boundary with commit/restore should reference shadow, not original. */

int g_hot __attribute__((annotate("milp_candidate")));
int g_cold __attribute__((annotate("milp_candidate")));

int main(void) {
    g_hot = 1;
    g_hot = g_hot + 2;
    g_hot = g_hot + 3;
    g_hot = g_hot + 4;

    g_cold = g_hot;

    return g_cold;
}
