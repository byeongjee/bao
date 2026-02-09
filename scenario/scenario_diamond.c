/* A5. Asymmetric if-else (diamond CFG).
 * One branch cheap, the other expensive.
 * Expected: Energy propagation handles both paths correctly. */

int scenario_diamond(int x) {
    int result;
    if (x > 0) {
        /* Cheap branch: ~3 instructions */
        result = x + 1;
    } else {
        /* Expensive branch: many instructions */
        int a = x - 1;
        int b = a * 2;
        int c = b + 3;
        int d = c - 4;
        int e = d * 5;
        int f = e + 6;
        int g = f - 7;
        int h = g * 8;
        int i = h + 9;
        int j = i - 10;
        int k = j * 11;
        int l = k + 12;
        int m = l - 13;
        int n = m * 14;
        int o = n + 15;
        result = o;
    }
    return result;
}
