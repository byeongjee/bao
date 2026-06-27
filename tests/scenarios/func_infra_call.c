// Caller invokes a DEFINED benchmark-infrastructure function.
//
// `debug_log` matches isBenchmarkInfrastructureFunction (debug_*), so the module
// driver skips solving it. For consistency, call isolation must NOT isolate the
// call (treat it as a helper) and StateAnalysis must allow it — otherwise the
// call would be isolated yet never summarized, stranding `main` (skipped). This
// fixture guards the whitelist alignment between isolation / StateAnalysis /
// driver-skip.
int a = 0;

__attribute__((noinline)) int debug_log(int x) {
    return x + 1;
}

int main(void) {
    a = debug_log(5);
    return a;
}
