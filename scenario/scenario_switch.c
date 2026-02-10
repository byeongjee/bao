/* A6. Multiple successors (switch statement).
 * Switch with 4 cases of varying cost.
 * Expected: Handles multi-successor CFG correctly. */

int main(void) {
    int sel = 2;
    int result;
    switch (sel) {
    case 0:
        /* Cheap */
        result = 1;
        break;
    case 1:
        /* Medium */
        result = sel + 2;
        result = result * 3;
        result = result - 4;
        break;
    case 2: {
        /* Expensive */
        int a = sel + 10;
        int b = a * 20;
        int c = b + 30;
        int d = c - 40;
        int e = d * 50;
        int f = e + 60;
        int g = f - 70;
        int h = g * 80;
        int i = h + 90;
        int j = i - 100;
        int k = j * 110;
        int l = k + 120;
        int m = l - 130;
        int n = m * 140;
        result = n;
        break;
    }
    case 3:
        /* Medium-cheap */
        result = sel * sel;
        break;
    default:
        result = 0;
        break;
    }
    return result;
}
