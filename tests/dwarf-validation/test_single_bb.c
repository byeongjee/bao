// Test: Single basic block (straight-line code)
// Expected: 1 BB

int test_single_bb(int a, int b) {
    int x = a + b;
    int y = x * 2;
    int z = y - a;
    return z;
}
