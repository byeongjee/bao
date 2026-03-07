/* AllocaInst (stack slot) as V_inelig gets MILP-optimized commit/restore.
 * g_candidate is annotated (V_elig), local_var is a stack alloca (V_inelig).
 * Phase 2 reads both before writing, making them live-in at the boundary.
 * Expected: @__nvm_alloca_local_var in .nvm; commit/restore memcpy at boundary. */

int g_candidate;
volatile int barrier;

int main(void) {
    /* Phase 1: write candidate and local stack var */
    int local_var = 42;
    g_candidate = local_var;
    int a = g_candidate + local_var;
    int b = a + 1;
    int c = b + 2;
    int d = c + 3;
    int e = d + 4;
    int f = e + 5;
    int g = f + 6;

    if (barrier) {
        /* Phase 2: READ local_var and candidate first (live-in) */
        int h = local_var + g_candidate;
        g_candidate = h;
        local_var = h + 1;
        int i = local_var + 9;
        int j = i + 10;
        int k = j + 11;
        int m = k + 12;
        int n = m + 13;
        return n;
    }

    return g;
}
