# PHI-Aware Boundary Insertion

## Problem

The MILP instrumenter (`CheckpointInstrumenter::insertRegionBoundaries`) inserts
commit stores, boundary calls, and restore loads at region boundary blocks. When
loop strip-mining + LCSSA canonicalization precedes the MILP pass, boundary blocks
contain PHI nodes that capture loop-exit values. The current algorithm ignores
these PHIs, causing restore loads to be dead code.

### Concrete example

After strip-mining a sum-reduction loop, the boundary block (`outer.latch`) looks
like:

```llvm
outer.latch:
  %add.lcssa.ol = phi [%add, %for.body]     ; captures final sum from inner loop
  %sum.05.fwd  = phi [%sum.05, %for.body]   ; carries state to next strip
```

The current instrumenter:
1. **Commit**: Uses SSAUpdater to resolve the reaching def of `%add` at the
   boundary. SSAUpdater walks predecessors and finds `%add` in `for.body`.
   Emits `store %add, @__nvm_ssa_1`. This bypasses the PHI.
2. **Restore**: Emits `%3 = load @__nvm_ssa_1`. Then SSAUpdater tries to
   rewrite uses of `%add` with `%3`. For the PHI use
   `%add.lcssa.ol = phi [%add, %for.body]`, `RewriteUseAfterInsertions` asks
   for the value at the end of `for.body` and gets `%add` (unchanged).
3. **Result**: `%3` has no users. LLVM eliminates it during codegen. After BOR
   recovery, registers are garbage and the committed value in FRAM is never
   loaded back.

### Root cause

The instrumenter operates on the original SSA values from `StateAnalysis` (e.g.,
`%add` defined in `for.body`). But at the boundary block, those values arrive
through PHIs. The instrumenter should work with whatever values are actually
present at the boundary point.

## Design

Replace the current 3-phase SSA approach (Phase 1 defer, Phase 2 SSAUpdater
commit, Phase 3 SSAUpdater restore) with a single-pass algorithm that works
directly with PHIs in the boundary block.

### Algorithm

For each boundary block `BB` (non-entry region starts):

**Step 1: Build PHI map.**

Scan PHIs in `BB`. Build a map from incoming value to PHI:

```
phiMap: Value* -> PHINode*
For each PHI in BB:
  For each incoming (value, block) pair:
    phiMap[value] = PHI
```

**Step 2: Emit commit stores.**

For each commit variable `V` from the MILP solution:

- **Global/alloca**: Emit commit memcpy (unchanged from current code).
- **SSA value**: Look up `phiMap[V]`.
  - If found: `commitVal = phiMap[V]` (the PHI itself).
  - If not found: `commitVal = V` (V must dominate BB; this handles the
    rare non-loop boundary case).
  - Emit: `store commitVal, @nvm_backup_V`
  - Record the store instruction for later (needed by step 4).

**Step 3: Emit boundary call.**

```
call @__region_boundary()
```

**Step 4: Emit restore loads and connect to uses.**

For each committed SSA variable `V` (reverse of step 2):

- Emit: `restoreVal = load @nvm_backup_V`
- If we committed a PHI (`commitVal` was a PHI from step 2):
  - `commitVal->replaceUsesWithIf(restoreVal, filter)` where `filter`
    excludes the commit store from step 2.
- If we committed `V` directly (no PHI):
  - Replace uses of `V` outside `BB` with `restoreVal`. This is the only
    case that may need SSAUpdater (for multi-block reach). In practice this
    is rare since boundaries typically occur at loop strip points which
    always have PHIs.

### Ordering constraint

`replaceUsesWithIf` must exclude the commit store. Otherwise the commit becomes
`store restoreVal, @nvm_backup` (circular: storing the load of itself).

```cpp
auto *commitStore = builder.CreateStore(commitVal, nvmBackup);
// ... boundary call ...
auto *restoreVal = builder.CreateLoad(backupTy, nvmBackup);
commitVal->replaceUsesWithIf(restoreVal, [commitStore](Use &U) {
    return U.getUser() != commitStore;
});
```

### Why this is safe

1. **Normal execution (no BOR):** `restoreVal = load @nvm_backup` reads the
   value just written by the commit store. So `restoreVal == commitVal` (the
   PHI). Replacing PHI uses with `restoreVal` is semantically equivalent.

2. **After recovery (BOR):** Registers are wiped. The PHI value is garbage.
   `restoreVal` loads the correct committed value from FRAM. Downstream uses
   (including back-edge successors in the outer loop) see the correct value.

3. **Inside loops:** The boundary block may be inside the outer strip-mined
   loop. Each iteration: PHI resolves to the inner loop's exit value, commit
   stores it, boundary fires, restore loads it. The back-edge carries
   `restoreVal` to the next iteration. This is correct because `restoreVal`
   equals the PHI value during normal execution.

4. **Commit store preserved:** The `replaceUsesWithIf` filter ensures the
   commit store still reads from the PHI, not from the restore load.

### What this eliminates

- **Phase 2 (SSAUpdater for commits):** No longer needed. The PHI IS the
  reaching definition at the boundary — no need to resolve it via SSAUpdater.
- **Phase 3 (SSAUpdater for restores):** No longer needed. `replaceUsesWithIf`
  directly connects the restore to all downstream uses.
- **The `pendingSSACommits` / `ssaRestoreDefs` data structures.**
- **The `allCommitInsts` tracking** (needed only to tell Phase 3 which
  instructions to skip).

### What stays the same

- Phase 1 handling of **globals and allocas** (commit/restore via memcpy).
  These don't have the PHI problem because they use pointer-based backup, not
  SSA values.
- The `addDebugMarkers_` counter increments.
- The overall structure: iterate over boundary blocks, emit commits before the
  boundary call, emit restores after.

### Edge cases

1. **No PHI for a commit variable:** The value dominates the boundary block
   directly (e.g., a value defined in a dominating block, not in a loop body).
   Commit and restore it directly. For the restore, replace uses of the
   original value outside its defining block with the restore load. This may
   need SSAUpdater as a fallback, but is expected to be rare.

2. **Multiple PHIs capturing the same value:** A boundary block could have
   multiple PHIs with the same incoming value (unlikely but possible). The
   `phiMap` would keep only one. In practice, LCSSA produces one PHI per
   escaping value.

3. **PHI with multiple incoming blocks:** If the boundary block has multiple
   predecessors, a PHI may have different incoming values from different blocks.
   The `phiMap` should map each incoming value separately. The commit should
   store the PHI (which resolves to the correct incoming value at runtime).

## Scope

This design covers only the instrumenter (`CheckpointInstrumenter::insertRegionBoundaries`).
It does NOT change:
- The MILP solver or its formulation
- `StateAnalysis` (which identifies commit/restore variables)
- The abstract CFG or energy model
- The `MILPSolution` data structure

The separate issue of values that are live across boundaries but not committed
by the MILP solver (e.g., `%sum.05.fwd`) is out of scope and will be handled
in the MILP solver.

## Testing

- All 34 existing MILP tests must continue to pass.
- The `scenario_ssa_ckpt` test specifically exercises SSA checkpoint/restore and
  should be verified to produce correct IR with the restore load connected.
- Add a new test case (or extend `scenario_ssa_ckpt`) that verifies the restore
  load is used by downstream instructions (not dead).
- Compile the `test` benchmark and verify the assembly contains a load from
  `@__nvm_ssa_*` after the boundary call, with the loaded value used by
  subsequent instructions.

## Files to modify

| File | Change |
|------|--------|
| `passes/src/milp/CheckpointInstrumenter.cpp` | Rewrite `insertRegionBoundaries` |
| `passes/include/milp/CheckpointInstrumenter.h` | Remove Phase 2/3 member fields if unused |
| `tests/scenarios/` | Add/update SSA restore test scenario |
