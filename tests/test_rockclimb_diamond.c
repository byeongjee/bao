/*
 * RockClimb Test: Diamond CFG (If-Else)
 * CFG: entry -> (condition) -> then/else -> merge -> exit
 *
 * Tests region partitioning with branching.
 * Both branches should be in the same region if their combined max energy fits E_safe.
 */

volatile int sink;

int test_rockclimb_diamond(int x, int condition) {
    volatile int result;

    // Entry block
    sink = x;

    // Condition block decides which path to take
    if (condition > 0) {
        // Then branch - ~15 instructions
        result = x + 1;
        result = result + 2;
        result = result + 3;
        result = result + 4;
        result = result + 5;
        sink = result;
    } else {
        // Else branch - ~20 instructions (heavier)
        result = x * 2;
        result = result * 3;
        result = result * 4;
        result = result - 1;
        result = result - 2;
        result = result - 3;
        result = result - 4;
        sink = result;
    }

    // Merge block
    result = result + 100;

    // Exit
    return result;
}
