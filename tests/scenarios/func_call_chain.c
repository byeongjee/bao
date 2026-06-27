// Transitive VIRTUAL propagation through a call chain: main -> a_fn -> b_fn.
//
// b_fn runs an inner loop too heavy for one charge, so it needs an internal
// checkpoint (VIRTUAL). a_fn calls b_fn, so a_fn transitively contains a
// (VIRTUAL) checkpoint and must itself fold VIRTUAL into main. This checks that
// the has-checkpoint property propagates up the bottom-up chain.

int g = 0;

void __loop_tripcount(int);

__attribute__((noinline)) int b_fn(void) {
    int s = 0;
    for (int i = 0; i < 1; i++) {
        __loop_tripcount(1);
        for (int j = 0; j < 100; j++) {
            __loop_tripcount(100);
            s += j * 3 + i;
        }
    }
    g = s;
    return s;
}

__attribute__((noinline)) int a_fn(void) {
    return b_fn() + 1;
}

int main(void) {
    return a_fn();
}
