/* Slide: MILP minimizes distributed checkpoint store cost.
 * The MILP objective includes the cost of saving live values at
 * checkpoint boundaries. Two boundary placements both give 2 regions:
 *   - Early (between Block 1 and Block 2): only 'a' is live → ~1 store
 *   - Late  (between Block 2 and Block 3): a,b,c,d,e live → ~5 stores
 * A greedy algorithm goes as far as possible → late boundary → 5 stores.
 * The MILP picks the early boundary to minimize total store cost.
 * Expected: 2 regions, fewer distributed checkpoints than greedy. */

volatile int b1, b2;

int main(void) {
    int x = 1;
    /* Block 1: define 'a', pad to force energy (~15) */
    int a = x * 3 + 7;
    int p1 = x + 1;
    int p2 = p1 + 2;
    int p3 = p2 + 3;
    int p4 = p3 + 4;
    int p5 = p4 + 5;
    int p6 = p5 + 6;
    int p7 = p6 + 7;
    int p8 = p7 + 8;

    if (b1) {
        /* Block 2: define b,c,d,e — these become live across late boundary */
        int b = a + 10;
        int c = b + 11;
        int d = c + 12;
        int e = d + 13;
        int q1 = e + 1;
        int q2 = q1 + 2;
        int q3 = q2 + 3;
        int q4 = q3 + 4;
        int q5 = q4 + 5;
        int q6 = q5 + 6;
        int q7 = q6 + 7;

        if (b2) {
            /* Block 3: use ALL of a,b,c,d,e */
            int result = a + b + c + d + e;
            int r1 = result + 1;
            int r2 = r1 + 2;
            int r3 = r2 + 3;
            int r4 = r3 + 4;
            int r5 = r4 + 5;
            int r6 = r5 + 6;
            int r7 = r6 + 7;
            int r8 = r7 + 8;
            return r8;
        }
        return q7;
    }
    return p8;
}
