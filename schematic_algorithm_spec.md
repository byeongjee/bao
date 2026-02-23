# SCHEMATIC algorithm specification (implementation-oriented)

This document specifies the **SCHEMATIC** compiler algorithm (**Si**multaneous **Che**ckpoint Placement and **M**emory Allocation **Ta**ilored for **I**nter**C**omputing) for intermittent/battery-free embedded systems.

It is intended to be **directly actionable for implementation** by coding agents.

---

## 1. Problem and guarantees

### 1.1 Objective

Given:
- a program running on an intermittent platform with
  - **Volatile Memory (VM)**: small, low-energy, lost on power failure
  - **Non-Volatile Memory (NVM)**: persistent, higher-energy accesses
- a capacitor energy buffer with usable energy per charge **`capacity`**
- a VM capacity **`vm_capacity_bytes`** (bytes)
- a **safe worst-case energy consumption (WCEC)** model

SCHEMATIC computes at **compile time**:

1) **Checkpoint placement**: which **CFG edges** are enabled checkpoint locations.

2) **Variable placement** (VM or NVM): where each `milp_candidate`-annotated global variable resides in each code region between checkpoints.

Goal: **minimize total energy** on the most frequently executed paths, while ensuring safety.

### 1.2 Runtime checkpointing strategy (required semantics)

At runtime, when reaching an **enabled checkpoint edge**:

1. **Save** volatile state (CPU registers + VM-resident variables that must persist) to NVM.
2. **Hibernate / wait** until the capacitor is **fully replenished**.
   - During waiting, system may sleep and periodically check capacitor voltage.
   - If power fails during waiting, on restart the system should continue waiting until full charge.
3. **Restore** saved state into registers/VM.
4. **Resume** execution.

This "save → recharge → restore → execute" model is central: it prevents re-execution and thus avoids memory anomalies.

### 1.3 Guarantees (under the WCEC + `capacity` assumptions)

SCHEMATIC is designed to ensure:

- **Forward progress**: between successive checkpoints, worst-case energy is ≤ `capacity`, and execution resumes only with a full capacitor.
- **Absence of memory anomalies** (by design): no re-execution means no duplicate NVM updates.
- **Best-effort energy minimization** on frequent paths.
- **VM capacity respected**: total live VM allocation ≤ `vm_capacity_bytes` at any time.

---

## 2. Inputs / outputs

### 2.1 Compile-time inputs

#### Program representation
- Function-level **CFG**: basic blocks (BBs) and directed edges (reuse `CFGAnalysis`).
- Loop information: natural loops and loop nesting (reuse LLVM `LoopInfo`).
- Call graph (non-recursive programs only).

#### Variable inventory
Only **global variables annotated with `milp_candidate`** are subject to VM/NVM placement decisions (same as the MILP pass, via `StateAnalysis`).

For each candidate variable `v`:
- `size(v)` in bytes (from `StateAnalysis::getVarSizeBytes`)
- stable NVM "home" location/symbol
- pointer-accessed / address-taken variables are forced to NVM

#### Energy model (WCEC)
Loaded from a **SCHEMATIC config JSON** (see §4 for the full field list). Shares the same parameter names as the MILP config where applicable.

#### Frequency data
- LLVM `BlockFrequencyInfo` and `BranchProbabilityInfo` for static block/edge frequency estimation (same source as MILP).

#### Annotations
- Loop maximum iteration count `max_it(loop)` (needed for conditional loop checkpointing). Obtained from `__loop_tripcount` annotation (same as RockClimb) or LLVM `ScalarEvolution::getSmallConstantMaxTripCount()` as fallback. If neither is available, this is a **fatal error**: the implementation must emit a diagnostic and abort analysis for the current function. All loops must have a trip count bound via annotation or ScalarEvolution.

### 2.2 Compile-time outputs

- `checkpoint_enabled[e] ∈ {true,false}` for each CFG edge `e`
- Memory allocation plan:
  - variable placement VM/NVM per interval (region between checkpoints)
  - VM offset assignment for VM variables per interval
- Transformed code:
  - inserted checkpoint save/hibernate/restore sequences on enabled edges
  - rewritten memory accesses to target VM vs NVM addresses per region
  - loop back-edge conditional checkpoint logic (if used)

---

## 3. Core representation and terminology

### 3.1 CFG and candidate checkpoints

- SCHEMATIC considers **every CFG edge** as a **candidate checkpoint location**.
- Let each candidate checkpoint be denoted `c_e` for CFG edge `e = (u → v)`.

### 3.2 Paths

- A **path** is an ordered sequence of basic blocks from function entry to an exit.
- SCHEMATIC analyzes paths iteratively, prioritizing frequent paths first.

### 3.3 Regions / intervals

- A **region** is code executed between successive **enabled checkpoints**.
- Memory allocation (VM/NVM) may change **only at checkpoints**.

---

## 4. Energy model interface (implementation contract)

### 4.1 Configuration parameters

SCHEMATIC uses a JSON config file with the following fields. Names are chosen to match the existing MILP config where the concept is identical.

| JSON field | Type | Description |
|---|---|---|
| `capacity` | double | Energy buffer capacity (energy per full charge) |
| `E_pro` | double | Board prologue energy (hardware init on power-up, **excluding** register/variable restore) |
| `E_epi` | double | Board epilogue energy (hardware poweroff, **excluding** register/variable save) |
| `N_reg` | unsigned | Number of CPU registers to save/restore at each checkpoint |
| `reg_store_energy` | double | Energy to store one register to NVM |
| `reg_restore_energy` | double | Energy to restore one register from NVM |
| `nvm_access_penalty` | double | Extra energy per memory access when targeting NVM vs VM (= `ΔE` per access) |
| `mem_store_energy_per_byte` | double | Energy per byte for VM→NVM copy (used for variable save) |
| `mem_restore_energy_per_byte` | double | Energy per byte for NVM→VM copy (used for variable restore) |
| `vm_capacity_bytes` | unsigned | VM (SRAM) capacity in bytes |
| `max_paths` | unsigned | Maximum number of paths to analyze per function (bounds compile time) |

All fields are required (no silent defaults).

> **Design note (register cost model):** SCHEMATIC uses a fixed `N_reg` blanket cost at every checkpoint, as described in the original paper. This is a deliberate simplification compared to MILP, which discovers live SSA values per block via `StateAnalysis` and only saves/restores registers that are actually live at each boundary. The fixed `N_reg` model is simpler to implement and analyze but may over-estimate register save/restore costs at some checkpoints.

### 4.2 Derived energy quantities

From the config, the following per-checkpoint and per-variable costs are computed:

**Register save/restore totals:**
- `E_save_regs = N_reg * reg_store_energy`
- `E_restore_regs = N_reg * reg_restore_energy`

**Per-variable save/restore:**
- `E_save_var(v) = mem_store_energy_per_byte * size(v)`
- `E_restore_var(v) = mem_restore_energy_per_byte * size(v)`

**NVM access penalty:**
The config provides a single `nvm_access_penalty` value representing the per-access energy difference between NVM and VM. This serves as both `ΔE_R` and `ΔE_W`:
- `ΔE_R = nvm_access_penalty` (energy saved per read by using VM instead of NVM)
- `ΔE_W = nvm_access_penalty` (energy saved per write by using VM instead of NVM)

> **Note:** The current config uses a single `nvm_access_penalty` for both reads and writes. If asymmetric read/write penalties are needed in the future, this can be split into `nvm_read_penalty` and `nvm_write_penalty`.

### 4.3 Block execution energy
- `E_exec_block(bb, placement_map)`:
  - Base instruction energy (from `EnergyEstimator` via `CFGAnalysis`, same as MILP)
  - Plus NVM access penalties for variables placed in NVM: `Σ_v (loads[bb][v] + stores[bb][v]) * nvm_access_penalty` for each NVM-placed variable `v`
  - Minus NVM access penalties for variables placed in VM (these accesses become cheaper)

**Important precondition:** if any basic block has WCEC > `capacity`, you MUST split it until each block fits. Reuse `RockClimbOptimizer::splitBlock()` (factor out to `common/` as a shared utility): split at the LLVM IR instruction boundary where cumulative instruction energy first reaches `capacity`.

---

## 5. Access counting and liveness

### 5.1 Read/write counts

Reuse `StateAnalysis::getLoadCount(BB, gv)` and `StateAnalysis::getStoreCount(BB, gv)` for per-block access counts of `milp_candidate` globals.

For each basic block `bb`, the existing infrastructure provides:
- `loads[bb][v]` = number of loads of `v` in `bb` (via `getLoadCount`)
- `stores[bb][v]` = number of stores to `v` in `bb` (via `getStoreCount`)

Then for an interval of blocks `I` (between checkpoints):
- `nR(v, I) = Σ_bb∈I loads[bb][v]`
- `nW(v, I) = Σ_bb∈I stores[bb][v]`

### 5.2 Liveness flags at interval boundaries

Checkpoint overhead can be reduced:

- If `v` is not used after the interval end checkpoint, you do **not** save it.
- If the first access to `v` after interval start checkpoint is a **write**, you do **not** restore it.

Implement interval-boundary flags:
- `live_start(v) ∈ {0,1}`: whether a restore is needed at interval start
- `live_end(v) ∈ {0,1}`: whether a save is needed at interval end

Then:
- `E_save_restore(v) = E_restore_var(v) * live_start(v) + E_save_var(v) * live_end(v)`

Implementation guidance:
- `live_end(v)` approximates "v is live-out of the interval" on that path.
- `live_start(v)` requires "first access" logic:
  - scan forward from interval start until the first access to `v`;
  - if first access is a read → `live_start(v)=1`
  - if first access is a write → `live_start(v)=0`

---

## 6. Allocation decision for one interval (inner optimizer)

### 6.1 Gain function

For each `milp_candidate` variable `v` referenced in the interval:

```
gain(v) = nvm_access_penalty * nW(v) + nvm_access_penalty * nR(v) - E_save_restore(v)
        = nvm_access_penalty * (nR(v) + nW(v)) - E_save_restore(v)
```

Interpretation:
- first term = energy saved by using VM for reads/writes (penalty avoided)
- last term = overhead of saving/restoring `v` at checkpoints

### 6.2 Greedy packing into VM

Because VM is limited, SCHEMATIC uses a greedy heuristic:

1. Consider only variables with `gain(v) > 0`.
2. Sort by decreasing **gain-to-size ratio**: `gain(v) / size(v)`.
3. Allocate in VM contiguously in that order until:
   - VM is full (`vm_capacity_bytes` exhausted), or
   - no positive-gain variables remain.

All other variables remain in NVM.

### 6.3 Addressing rules

- Every variable has a single NVM "home" address.
- If `v` is placed in VM within an interval, assign a VM offset that is:
  - stable within the interval
  - may differ between intervals

All loads/stores inside an interval must use the correct address for that interval.

---

## 7. Reachable Checkpoint Graph (RCG) for one CFG path

SCHEMATIC reduces joint checkpoint+allocation selection to shortest path in an auxiliary graph.

### 7.1 RCG definition

Given a CFG path `P`:

- Nodes:
  - one node for each candidate checkpoint location on `P` (in path order)
  - two virtual nodes: `start`, `end`

- Directed edge `(c_i → c_j)` exists (with `i < j`) iff there exists a variable placement for the code between `c_i` and `c_j` whose worst-case energy fits within budget.

Each RCG edge stores:
- `edge_cost = E_interval(i,j)` (energy)
- `edge_allocation` (placement map + VM offsets)

### 7.2 Interval energy cost

For interval `[c_i, c_j]` (code between checkpoints):

```
E_interval(i,j) =
    E_restore_at(c_i)
  + Σ_{bb in interval} E_exec_block(bb, placement)
  + E_save_at(c_j)
```

Where:
- `E_restore_at(c_i) = E_pro + N_reg * reg_restore_energy + Σ_{v in VM} mem_restore_energy_per_byte * size(v) * live_start(v)`
  (board prologue = hardware init on power-up, then restore registers and variables)
- `E_save_at(c_j) = E_epi + N_reg * reg_store_energy + Σ_{v in VM} mem_store_energy_per_byte * size(v) * live_end(v)`
  (save registers and variables, then board epilogue = hardware shutdown)

### 7.3 Feasibility budget

Normally, require:

```
E_interval(i,j) ≤ capacity
```

However, when analyzing a path segment that overlaps already-analyzed code, SCHEMATIC tightens budgets using `E_left` and `E_to_leave` (see §8).

### 7.4 Selecting checkpoints and allocations on the CFG path

Any `start → … → end` path in the RCG corresponds to:
- a valid set of enabled checkpoints on the CFG path
- a valid per-interval allocation

SCHEMATIC chooses the **minimum-energy** RCG path:
- Paper uses Dijkstra.
- Since RCG edges only go forward, the RCG is a DAG; you may implement shortest path via DP/topological order.

Enable checkpoints corresponding to nodes on the chosen RCG path; disable the rest (unless previously enabled by earlier decisions).

### 7.5 RCG infeasibility

If no `start → end` path exists in the RCG (no feasible interval partition exists for this path), the implementation MUST emit a diagnostic warning identifying the infeasible path and abort analysis for the current function. This can occur when a sequence of blocks has cumulative energy exceeding `capacity` and no intermediate candidate checkpoint edges can break it into feasible intervals. The block-splitting precondition (§4.3) should prevent single-block infeasibility, but multi-block sequences without branch edges remain possible.

---

## 8. Multi-path iterative analysis and overlap handling

SCHEMATIC analyzes paths in decreasing frequency, but decisions are **greedy and final**.

### 8.1 Frequency-ordered path enumeration

Paths are enumerated using LLVM's `BranchProbabilityInfo` (static analysis, same frequency source as MILP's `BlockFrequencyInfo`).

**Algorithm (greedy most-probable-path enumeration on acyclic CFG):**

1. Remove back-edges from the CFG (loops are handled separately in §10). The result is a DAG.
2. Seed a priority queue with the entry block (path probability = 1.0).
3. At each step, extend the highest-probability partial path by following all successor edges, each weighted by `BranchProbabilityInfo::getEdgeProbability`. Path probability = product of edge probabilities along the path.
4. When a path reaches an exit block, emit it as a complete path.
5. Stop when all blocks are covered OR `max_paths` paths have been emitted.
6. **Coverage paths**: for any remaining uncovered blocks, for each uncovered block `B`, find the shortest (fewest hops) path from entry through `B` to an exit. Rank these by frequency and emit them.

Paths are analyzed in decreasing frequency order.

### 8.2 Analyze only new segments

When analyzing a path `p`, only segments containing at least one **unprocessed basic block** are actually optimized. Overlapping segments inherit earlier decisions.

### 8.3 Per-basic-block metadata

For each basic block already "decided", maintain:
- `mem_alloc(bb)` : allocation context at that point (or sufficient info to constrain interval allocation)
- `E_left(bb)` : worst-case energy remaining **after** executing `bb`
- `E_to_leave(bb)` : minimum energy that must be available **when entering** `bb` to safely reach subsequent checkpoints already fixed

### 8.4 Budget adjustment for overlapped segments

When building an RCG for a path segment that overlaps earlier-fixed decisions:

- For edges **originating from `start`**, use:

```
budget = E_left(start_bb)
```

instead of `capacity`.

- For edges **ending at `end`**, use:

```
budget = capacity - E_to_leave(end_bb)
```

### 8.5 Monotonic propagation invariants

After each new path analysis:
- `E_left(bb)` can only **decrease**
- `E_to_leave(bb)` can only **increase**

This ensures conservativeness as more paths are analyzed.

---

## 9. Function calls (interprocedural constraints)

> **Scope for v1:** The initial implementation is **intraprocedural only** (function-at-a-time), matching MILP and RockClimb. Functions with unresolved call sites are handled via `StateAnalysis` strict mode (same as MILP). The interprocedural analysis described below is deferred to a future version.

### 9.1 Order and restriction

- Analyze functions in **reverse topological order** of the call graph (callee before caller).
- Recursion is not supported.

### 9.2 Context-independence

Decisions for a function (checkpoints + allocations) must be **context-independent**:
- identical regardless of call site
- once decided, never reconsidered

### 9.3 Modeling calls in callers

When analyzing caller `f_caller` containing call to `f_callee`:

#### Case A: callee has no checkpoints
- Callee uses one allocation for all blocks (no allocation changes).
- Treat the call as a single "macro basic block" with known WCEC under that allocation.

#### Case B: callee has checkpoints
- Caller must account for:
  - energy/allocation up to callee's first checkpoint(s)
  - allocation and remaining energy at callee exit
- Impose: **single exit allocation** for the callee, regardless of number of exits.

---

## 10. Loop handling with conditional back-edge checkpointing

SCHEMATIC handles **natural loops** and traverses loop nesting bottom-up (inner first).

Assume a loop with:
- header block `H`
- latch block `L`
- back-edge `L → H`
- annotated `max_it` iterations

### 10.1 Step 1: analyze one iteration (back-edge removed)

- Temporarily remove the back-edge.
- Analyze loop body like an acyclic CFG path set.
- This yields:
  - memory allocation inside loop body
  - checkpoints inside body only if needed to fit in `capacity`

### 10.2 Step 2: decide back-edge policy

If `alloc(H) != alloc(L)`:
- MUST enable a checkpoint on every back-edge traversal to allow allocation change between iterations.

Else:
- Compute per-iteration energy:

```
E_loop = E_left(H) - E_left(L)
```

- Compute checkpoint overhead energy:

```
E_ckpt = E_epi + E_pro + N_reg * (reg_store_energy + reg_restore_energy)
       + Σ_{v in VM} (mem_store_energy_per_byte + mem_restore_energy_per_byte) * size(v)
```

- Compute number of iterations executable per charge:

```
num_it = floor((capacity - E_ckpt) / E_loop)
```

- If `num_it ≥ max_it`:
  - no back-edge checkpoint needed (entire loop fits in one charge)

- Else:
  - insert **conditional checkpoint** on the back-edge every `num_it` iterations, **including the 0-th iteration** (i.e., before the loop body first executes)

The `num_it = floor((capacity - E_ckpt) / E_loop)` formula reserves energy for the checkpoint sequence itself. The 0-th iteration checkpoint ensures the loop always starts with a fully charged capacitor.

### 10.3 Runtime structure for conditional checkpoint

At the loop preheader (before entering the loop):
- initialize loop counter to 0

At latch `L` before jumping to header:
- if `counter % num_it == 0`:
  - execute checkpoint save/hibernate/restore (full recharge)
- increment loop counter
- continue to header

Since the counter starts at 0 and the check is `counter % num_it == 0`, the checkpoint fires **before the first iteration** (0-th) and then every `num_it` iterations thereafter. This guarantees a full capacitor charge at the start of each group of `num_it` iterations.

---

## 11. Compiler transformation requirements

### 11.1 Checkpoint insertion points

SCHEMATIC decides checkpoint locations on **CFG edges**. Implementation options:

- Split the edge by inserting a new basic block `BB_chkpt` so you can place code in the CFG.
- Or use IR-level "edge instrumentation" (phi-safe) depending on compiler IR.

Checkpoint code sequence (in order):
1. save registers (cost: `N_reg * reg_store_energy`)
2. save VM variables selected for the ending region and live at end (cost: `mem_store_energy_per_byte * size(v)` each)
3. execute board epilogue — hardware shutdown (`E_epi`)
4. hibernate until full charge
5. execute board prologue — hardware init on power-up (`E_pro`)
6. restore registers (cost: `N_reg * reg_restore_energy`)
7. restore VM variables selected for the starting region and live at start (cost: `mem_restore_energy_per_byte * size(v)` each)

### 11.2 Memory access rewriting

For each load/store of a `milp_candidate` variable `v`, at each program point:
- determine placement of `v` in the current region (`VM` or `NVM`)
- rewrite address to:
  - VM slot (region-specific offset) if VM
  - NVM home address if NVM

### 11.3 Pointer-accessed variables (optional restriction)

To match the referenced implementation:
- force variables accessed through pointers to remain in NVM (address stability)

---

## 12. Core pseudocode

### 12.1 High-level driver

```text
SCHEMATIC(function F):
  // v1: intraprocedural (function-at-a-time, same as MILP/RockClimb)

  build CFG via CFGAnalysis
  collect milp_candidate globals, sizes, pointer flags (via StateAnalysis)
  collect per-bb access counts and energy components
  ensure each basic block WCEC ≤ capacity (split via splitBlock if needed)

  analyze_loops_bottom_up(F)

  paths = enumerate_paths(F, max_paths)   // §8.1 algorithm using BranchProbabilityInfo

  analyzed_blocks = {}
  for path in paths in decreasing_frequency:
      if path contains any block not in analyzed_blocks:
          analyze_path_with_RCG(F, path)
          mark newly decided blocks as analyzed

  if any reachable block not in analyzed_blocks:
      emit warning: incomplete coverage
```

### 12.2 Path analysis via RCG

```text
analyze_path_with_RCG(f, CFG_path P):

  C = ordered candidate checkpoint locations (edges) along P
  nodes = [start] + C + [end]

  for each i < j in nodes:
      interval_blocks = blocks_between(P, i, j)

      placement = best_allocation(interval_blocks, constraints_from_fixed_blocks)
      E = interval_energy(i, j, interval_blocks, placement)

      budget = capacity
      if i == start and interval starts inside fixed region:
          budget = E_left(start_bb)
      if j == end and interval ends inside fixed region:
          budget = capacity - E_to_leave(end_bb)

      if E ≤ budget:
          add RCG edge (i -> j) with weight E and store placement

  shortest_path = shortest_path_RCG(start, end)

  if shortest_path does not exist:
      emit warning: "infeasible path — no valid checkpoint partition"
      abort analysis for this function     // §7.5

  enable checkpoints on CFG edges corresponding to selected nodes
  record per-region allocation decisions from stored placements

  update per-bb mem allocation, E_left, E_to_leave conservatively
```

### 12.3 Interval allocation (gain-based packing)

```text
best_allocation(interval, constraints):

  remaining_vm = vm_capacity_bytes - vm_bytes_reserved_by_constraints
  placement = constraints.fixed_placements

  for each milp_candidate variable v referenced in interval and not fixed:
      if pointer_accessed(v): placement[v]=NVM; continue

      nR = Σ loads[bb][v]       // via StateAnalysis::getLoadCount
      nW = Σ stores[bb][v]      // via StateAnalysis::getStoreCount
      live_start, live_end = compute_liveness_flags(v, interval boundaries)

      Esr = mem_restore_energy_per_byte*size(v)*live_start
          + mem_store_energy_per_byte*size(v)*live_end
      gain = nvm_access_penalty*(nR + nW) - Esr

      if gain > 0:
          candidates.add(v, gain, size(v), gain/size(v))

  sort candidates by decreasing gain/size
  for v in candidates:
      if size(v) ≤ remaining_vm:
          placement[v]=VM
          assign_vm_offset(v)
          remaining_vm -= size(v)
      else:
          placement[v]=NVM

  return placement
```

---

## 13. Correctness invariants for implementation

Your implementation should enforce these invariants:

1. **Energy feasibility**:
   - For every enabled-checkpoint-to-next-enabled-checkpoint interval, worst-case energy ≤ `capacity` (or adjusted budgets for overlap segments).

2. **Allocation change only at checkpoints**:
   - If `alloc(bb1) != alloc(bb2)` across edge `(bb1→bb2)`, that edge must have an enabled checkpoint.

3. **VM capacity**:
   - For every region, sum of `size(v)` for VM-placed variables ≤ `vm_capacity_bytes`.

4. **Save/restore soundness**:
   - A variable in VM must be restored at region start iff `live_start(v)=1`.
   - It must be saved at region end iff `live_end(v)=1`.

---

## 14. Implementation notes and engineering choices

- **RCG construction complexity** is quadratic in number of candidate checkpoints on a path (all pairs).
- RCG is a DAG → DP shortest path is simpler than Dijkstra.
- Variable allocation is heuristic (greedy by gain/size), not global knapsack; it is intended to be fast and effective.
- If WCEC is inaccurate and a power failure happens between checkpoints, the paper discusses detecting repeated restart from the same checkpoint and resetting to initial state; optionally implement a similar safeguard.
- **Reuse of existing infrastructure**: `CFGAnalysis`, `StateAnalysis` (milp_candidate detection, access counts, liveness, variable sizes), `EnergyEstimator` (per-block base costs), and `CheckpointInstrumenter` (code transformation) can be shared with the MILP pass.

---

## 15. Suggested internal data schema (optional)

Represent decisions in a form easy to consume by codegen:

```json
{
  "functions": {
    "f": {
      "checkpoints": { "edge_id": true, "...": false },
      "regions": [
        {
          "id": "r1",
          "from_checkpoint": "c_ab",
          "to_checkpoint": "c_cd",
          "vm_layout": { "varA": 0, "varB": 12 },
          "placement": { "varA": "VM", "varB": "VM", "varC": "NVM" }
        }
      ]
    }
  }
}
```

---

## 16. Testing strategy (high-level)

1. Unit test gain computation and liveness boundary flags.
2. Unit test VM packing (ratio sort + contiguous offsets).
3. Property tests:
   - any allocation change implies checkpoint
   - VM size constraints never violated
4. Energy feasibility tests with synthetic CFGs where feasibility is known.
5. End-to-end emulator tests:
   - periodic power failures with different `TBPF` values
   - assert termination and compare energy accounting with baselines

---

## Appendix A: Parameter cross-reference with MILP config

### Shared parameters (same JSON field name, same semantics)

| Config JSON field | MILP | SCHEMATIC | RockClimb |
|---|---|---|---|
| `capacity` | Yes | Yes | No (uses `E_input`) |
| `E_pro` | Yes | Yes | No |
| `E_epi` | Yes | Yes | No |
| `reg_store_energy` | Yes | Yes | No |
| `reg_restore_energy` | Yes | Yes | Yes |
| `nvm_access_penalty` | Yes | Yes | No |
| `mem_store_energy_per_byte` | Yes | Yes | No |
| `mem_restore_energy_per_byte` | Yes | Yes | No |
| `vm_capacity_bytes` | Yes | Yes | No |

### SCHEMATIC-only parameters (not in MILP config)

| Config JSON field | Description |
|---|---|
| `N_reg` | Fixed number of CPU registers to save/restore (also in RockClimb config). MILP discovers registers from IR instead. |
| `max_paths` | Maximum paths to enumerate per function. Bounds compile time. |

### MILP parameters NOT used by SCHEMATIC

| MILP Config field | Reason not needed |
|---|---|
| `q_reboot_probability` | SCHEMATIC implicitly assumes worst-case q=1.0. For evaluation, pin MILP to q=1.0 as well. |
| `loop_strip_mining_enabled` | SCHEMATIC has its own loop handling (§10: conditional back-edge checkpointing). |
| `loop_strip_mining_margin_percent` | Same as above. |

### Derived quantities

| Concept | Derivation |
|---|---|
| Total register save cost | `N_reg * reg_store_energy` |
| Total register restore cost | `N_reg * reg_restore_energy` |
| Per-variable save cost | `mem_store_energy_per_byte * size(v)` |
| Per-variable restore cost | `mem_restore_energy_per_byte * size(v)` |
| NVM access penalty (ΔE_R = ΔE_W) | `nvm_access_penalty` (single value for both reads and writes) |
| Per-block access counts | `StateAnalysis::getLoadCount`, `getStoreCount` |
| Per-block base energy | `EnergyEstimator` via `CFGAnalysis` |
| Variable sizes | `StateAnalysis::getVarSizeBytes` |
| Loop trip count (`max_it`) | `__loop_tripcount` annotation or `ScalarEvolution` fallback |

---
