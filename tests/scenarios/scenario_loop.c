/* A3. Boundary avoids hot loop body.
 * Loop with moderate body cost; boundary should land outside the loop body
 * since placing it inside would multiply overhead by iteration count.
 * Expected: No boundary inside loop body. */

void __loop_tripcount(int);

int main(void) {
    int n = 10;
    int sum = 0;
    for (int i = 0; i < n; i++) {
        __loop_tripcount(10);
        sum += i * 2 + 1;
    }
    return sum;
}
