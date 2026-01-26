// Test: Diamond CFG (if/else)
// Expected: 4 BBs (entry, then, else, merge)

int test_if_else(int x) {
    int result;
    if (x > 0) {
        result = x + 10;
    } else {
        result = x - 10;
    }
    return result;
}
