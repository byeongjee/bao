/*
 * Test: Multiple Exit Blocks (Early Return)
 * CFG: entry -> check -> error_exit (return -1)
 *                     -> process -> success_exit (return 0)
 *
 * Tests exit constraint on MULTIPLE exit blocks.
 * Both error_exit and success_exit must satisfy: y[b] + Cost(b) <= capacity
 * Uses volatile to prevent optimization.
 */

int test_early_return(volatile int* ptr, volatile int n) {
    volatile int result;

    // Entry: validation
    if (ptr == 0) {
        // Error exit 1: ~8 instructions
        result = -1;
        result = result - 1;
        result = result - 2;
        result = result - 3;
        return result;
    }

    if (n < 0) {
        // Error exit 2: ~8 instructions
        result = -2;
        result = result - 4;
        result = result - 5;
        result = result - 6;
        return result;
    }

    // Process: main work
    result = *ptr;
    result = result + n;
    result = result * 2;
    result = result + 1;
    result = result + 2;
    result = result + 3;

    return result;  // Success exit
}
