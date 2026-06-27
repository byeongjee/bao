// A call inside the caller's loop body (validates D5: the callee summary is
// folded onto the call site BEFORE loop analysis, so the loop is costed with the
// callee's energy baked in — not as a near-free call).
//
// step() is a noinline leaf (DISABLED fold). main calls it once per iteration.

int a = 0;

void __loop_tripcount(int);

__attribute__((noinline)) int step(int x) {
    return x * 3 + 1;
}

int main(void) {
    for (int i = 0; i < 10; i++) {
        __loop_tripcount(10);
        a = step(a);
    }
    return a;
}
