// Multi-function inter-procedural fixture (VIRTUAL regime).
//
// heavy() runs an inner loop of ~100 iterations whose total energy far exceeds
// the capacity, so heavy needs an internal (back-edge) checkpoint. A callee that
// contains a checkpoint folds into its caller as VIRTUAL: the call site becomes a
// wall (fixed call_entry/call_exit barriers), so main only has to fit the bounded
// slices around the call -- not heavy's whole energy. (If the call were instead
// folded transparently/DISABLED, main would have to fit heavy's entire energy in
// one interior block and would be infeasible.)
//
// heavy is noinline so the call survives to be isolated.

void __loop_tripcount(int);

int g_result;

__attribute__((noinline)) int heavy(void) {
    int sum = 0;
    for (int i = 0; i < 1; i++) {
        __loop_tripcount(1);
        for (int j = 0; j < 100; j++) {
            __loop_tripcount(100);
            sum += j * 3 + i;
        }
    }
    g_result = sum;
    return sum;
}

int main(void) {
    int r = heavy();
    return r;
}
