/*
 * RockClimb Test: PHI-Defined Live-Out Values
 *
 * Tests that PHI-defined values are correctly checkpointed by distributed
 * checkpointing. A PHI node at a control-flow merge defines a value that
 * may be live-out of its region -- if excluded from Def_r, it will never
 * appear in Def_r ∩ LiveOut_r and won't be saved to NVM.
 *
 * Structure (after mem2reg):
 *   entry → if.then (expensive_a) | if.else (expensive_b) → if.end (PHI merge)
 *   → if.then2 (checkpoint_barrier call → mandatory boundary) → return
 *
 * The call to checkpoint_barrier forces a region boundary (call sites are
 * mandatory boundaries in RockClimb). The PHI value %result.0 is defined
 * in if.end (Region 2) and used in if.then2 (Region 3), making it live-out.
 *
 * With cond=0: the else path is taken, PHI resolves to expensive_b(x).
 * %call1 (from expensive_b) is in the same region as the PHI → NOT live-out
 * independently. Only the PHI itself is live-out across the boundary — so
 * this path specifically tests whether PHI-defined values are checkpointed.
 *
 * Compile: clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone
 * Then:    opt -passes=mem2reg  (promotes stack vars to SSA/PHI form)
 */

volatile int sink;

int expensive_a(int x) {
    int acc = x;
    acc = acc + 1;  acc = acc * 2;  acc = acc + 3;
    acc = acc + 4;  acc = acc * 2;  acc = acc + 5;
    acc = acc + 6;  acc = acc * 2;  acc = acc + 7;
    acc = acc + 8;  acc = acc * 2;  acc = acc + 9;
    sink = acc;
    return acc;
}

int expensive_b(int x) {
    int acc = x;
    acc = acc - 1;  acc = acc * 3;  acc = acc - 2;
    acc = acc - 3;  acc = acc * 3;  acc = acc - 4;
    acc = acc - 5;  acc = acc * 3;  acc = acc - 6;
    acc = acc - 7;  acc = acc * 3;  acc = acc - 8;
    sink = acc;
    return acc;
}

/* Simple function whose call forces a mandatory region boundary */
int checkpoint_barrier(int x) {
    sink = x;
    return x;
}

int test_rockclimb_phi_liveout(int x, int cond) {
    int result;

    /* Branch: produces a PHI at the merge point after mem2reg */
    if (cond) {
        result = expensive_a(x);
    } else {
        result = expensive_b(x);
    }
    /* merge: result = PHI [expensive_a, if.then], [expensive_b, if.else] */

    /* Computation in the merge block (no function calls → no forced boundary) */
    int w = result + 1;
    w = w * 2;
    sink = w;

    /* Branch to a block with a function call → mandatory region boundary.
     * The PHI value 'result' is used after this boundary → live-out. */
    if (w > 0) {
        int z = checkpoint_barrier(w);
        return result + z;   /* uses PHI-defined 'result' in new region */
    }
    return result;
}
