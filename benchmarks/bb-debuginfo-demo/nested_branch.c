// Nested branches: more complex CFG
int classify(int x, int y) {
    if (x > 0) {
        if (y > 0) {
            return 1;  // quadrant I
        } else {
            return 4;  // quadrant IV
        }
    } else {
        if (y > 0) {
            return 2;  // quadrant II
        } else {
            return 3;  // quadrant III
        }
    }
}
