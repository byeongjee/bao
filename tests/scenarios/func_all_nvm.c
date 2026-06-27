// Multi-function inter-procedural fixture (DISABLED regime).
//
// Mirrors the reference function_all_nvm_1 call structure: main -> f, main -> g,
// g -> f, with f a leaf. At -O0 (with -disable-O0-optnone) there is no inlining,
// so the calls survive to be isolated by schematic-isolate. With a large energy
// budget every function fits in a single region (checkpoint-free), so every call
// folds in the DISABLED regime: the callee's whole energy is baked into its
// call_entry block and the caller flows straight through.
//
// Helpers are marked noinline so the calls cannot be flattened even if an
// optimization level is raised.

int a = 0;

__attribute__((noinline)) int f(void) {
    return a + 3;
}

__attribute__((noinline)) int g(void) {
    return f() + 1;
}

int main(void) {
    a = f();
    a = g();
    return a;
}
