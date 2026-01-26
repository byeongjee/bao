// Test: Multiple functions
// Expected: Each function has its own BBs

int helper(int x) {
    if (x < 0) {
        return -x;
    }
    return x;
}

int test_two_functions(int a, int b) {
    int abs_a = helper(a);
    int abs_b = helper(b);
    return abs_a + abs_b;
}
