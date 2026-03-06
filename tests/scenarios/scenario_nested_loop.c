/* A4. Nested loop frequency.
 * Inner loop is much hotter than outer; optimizer should avoid placing
 * boundaries inside the inner loop body.
 * Expected: No boundary inside inner loop body. */

int main(void) {
    int n = 5, m = 5;
    int sum = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < m; j++) {
            sum += i + j;
        }
    }
    return sum;
}
