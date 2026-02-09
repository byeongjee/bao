/* E2. Single block exceeds capacity.
 * One basic block with energy > capacity (25), making placement infeasible.
 * Expected: Error "exceed energy capacity". */

int litmus_infeasible(int x) {
    int a = x + 1;
    int b = a + 2;
    int c = b + 3;
    int d = c + 4;
    int e = d + 5;
    int f = e + 6;
    int g = f + 7;
    int h = g + 8;
    int i = h + 9;
    int j = i + 10;
    int k = j + 11;
    int l = k + 12;
    int m = l + 13;
    int n = m + 14;
    int o = n + 15;
    int p = o + 16;
    int q = p + 17;
    int r = q + 18;
    int s = r + 19;
    int t = s + 20;
    int u = t + 21;
    int v = u + 22;
    int w = v + 23;
    int y = w + 24;
    int z = y + 25;
    int aa = z + 26;
    int bb = aa + 27;
    int cc = bb + 28;
    int dd = cc + 29;
    int ee = dd + 30;
    int ff = ee + 31;
    int gg = ff + 32;
    int hh = gg + 33;
    int ii = hh + 34;
    int jj = ii + 35;
    return jj;
}
