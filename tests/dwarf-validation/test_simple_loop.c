// Test: Simple loop
// Expected: 3-4 BBs (entry/preheader, loop header, loop body, exit)

int test_simple_loop(int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}
