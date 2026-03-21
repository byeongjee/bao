# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MILP-based checkpoint insertion tool for intermittent computing. Analyzes LLVM IR, builds control flow graphs with energy cost estimates, and uses Gurobi to solve an optimization problem that determines optimal checkpoint placements minimizing runtime overhead while satisfying energy constraints. Also includes a machine-level RockClimb (PFI) baseline that operates post-register-allocation.

## Build Commands

```bash
# Set environment variables
export LLVM_DIR=/path/to/llvm-project/build
export GUROBI_HOME=/path/to/gurobi  # e.g., /Library/gurobi1300/macos_universal2

# Build
cd passes && mkdir -p build && cd build
cmake .. -DLLVM_DIR=$LLVM_DIR/lib/cmake/llvm
make
```

Output: `passes/build/CheckpointPass.so` (main plugin), `passes/build/bb-debuginfo/BBDebugInfoPass.so`, `passes/build/bb-energy-analyzer/bb-energy-analyzer`

## Running Passes

```bash
# Compile C to LLVM IR (must use -Xclang -disable-O0-optnone for -O0 code)
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone input.c -o input.ll

# MILP checkpoint insertion (-passes=checkpoint and -passes=milp are aliases)
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=checkpoint \
    -energy-config=./benchmarks/sample_energy_config_ir.json \
    -milp-config=./benchmarks/sample_milp_config.json \
    -S input.ll -o instrumented.ll

# RockClimb (PFI baseline — machine-level, post-regalloc via llc)
# See: ckpt compile rockclimb --help
```

### Pass Pipeline

The `checkpoint`/`milp` pass name registers a pipeline of four passes:
`LoopSimplifyPass → LCSSAPass → LoopStripMiningPass → MILPCheckpointPass`

Loop canonicalization (LoopSimplify + LCSSA) is required before LoopStripMining can transform loops.

### CLI Options

| Option | Description |
|--------|-------------|
| `-energy-config=<path>` | Energy estimator config JSON (required for all passes) |
| `-milp-config=<path>` | MILP optimization parameters JSON (required for MILP) |
| `-rockclimb-config=<path>` | RockClimb parameters JSON (machine pass, via llc) |
| `-milp-accept-feasible` | Accept feasible (non-optimal) MILP solutions |
| `-loop-strip-mining-enabled` | Enable loop strip-mining before MILP (also settable in milp-config) |
| `-checkpoint-function=<name>` | Checkpoint function name (default: `__checkpoint`) |

## Test Suite

Tests use **pytest** via **uv**. Default options (`-v -n auto`) are configured in `pyproject.toml` via `addopts`.

```bash
# Run all tests
uv run pytest tests/

# Run by category (pytest marks)
uv run pytest tests/ -m milp            # MILP pass tests + scenarios
uv run pytest tests/ -m rockclimb       # RockClimb pass tests
uv run pytest tests/ -m schematic       # SCHEMATIC pass tests
uv run pytest tests/ -m energy_validation  # Energy validation runtime tests
uv run pytest tests/ -m vm_nvm          # VM/NVM placement tests

# Shell wrapper (same flags)
./tests/run_tests.sh                # all tests
./tests/run_tests.sh --milp         # MILP only
./tests/run_tests.sh --rockclimb    # RockClimb only
./tests/run_tests.sh --schematic    # SCHEMATIC only
./tests/run_tests.sh --validate     # Energy validation only

# DWARF mapping validation (assembly-based workflow, separate)
./tests/dwarf-validation/validate_dwarf.sh
```

### Test file layout

| File | What it tests |
|------|---------------|
| `test_milp.py` | Basic MILP pass (11 cases: success, infeasible, missing bb-freq) |
| `test_milp_vm_nvm.py` | VM/NVM placement enforcement, overflow, basic placement |
| `test_rockclimb.py` | RockClimb machine-level pass (post-regalloc) |
| `test_scenarios.py` | 20 MILP scenarios with structural IR assertions |
| `test_schematic.py` | SCHEMATIC full trace pipeline (7 scenarios) |

Requires `passes/build/CheckpointPass.so` to be built first. Test configs live in `tests/` and `tests/scenarios/configs/`.

## Scripts — `ckpt` Python Package

The `scripts/ckpt/` package provides the compilation, benchmarking, and device interaction toolchain as a Python CLI. Install with `uv sync` (adds `click` dependency). Run via `python -m ckpt` or the `ckpt` entry point.

### CLI Commands

```bash
# Compilation pipelines (INPUT can be a benchmark name or path to .c file)
ckpt compile milp           INPUT --cap CAP [--link] [--estimator-mode assembly|ir] [--save-temps] [--halt-mode nop|bor|lpm4] [--cpu-freq 1|8|16] [--device-debug] [--accumulate-keys FILE] ...
ckpt compile rockclimb      INPUT --cap CAP [--link] [--no-precomputed-energy] [--save-temps] [--halt-mode nop|bor|lpm4] [--cpu-freq 1|8|16] [--device-debug] [--accumulate-keys FILE] ...
ckpt compile schematic      INPUT --cap CAP [--link] [--trace-file FILE] [--trace-only] [--save-temps] [--halt-mode nop|bor|lpm4] [--cpu-freq 1|8|16] [--device-debug] [--accumulate-keys FILE] ...
ckpt compile uninstrumented INPUT [--link] [--save-temps] [--halt-mode nop|bor|lpm4] [--cpu-freq 1|8|16] [--device-debug] ...
# Explicit config paths also accepted: -e ENERGY_CONFIG -m/-c/-s ALGO_CONFIG

# Benchmark runners (compile + flash + NVM readback → CSV)
ckpt bench milp            [BENCHMARKS...] [--cap 1uF] [--debug-counters] [--halt-mode] [--estimator-mode] [--accumulate-keys FILE] [-o CSV]
ckpt bench rockclimb       [BENCHMARKS...] [--cap 1uF] [--debug-counters] [--halt-mode] [--accumulate-keys FILE] [-o CSV]
ckpt bench schematic       [BENCHMARKS...] [--cap 1uF] [--debug-counters] [--halt-mode] [--estimator-mode] [--trace-config] [--accumulate-keys FILE] [-o CSV]
ckpt bench uninstrumented  [BENCHMARKS...] [--cpu-freq] [-o CSV]

# Semantic verification (defaults to --halt-mode bor to exercise checkpoint/restore under resets)
ckpt verify milp       [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--estimator-mode] [--cpu-freq]
ckpt verify rockclimb  [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--cpu-freq]
ckpt verify schematic  [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--estimator-mode] [--cpu-freq]

# Analysis
ckpt analyze strip-mining LOG_FILE [-o CSV]
ckpt analyze plot       CSV_DIR [--metric M] [--algorithms A...]

# Device interaction
ckpt device read-serial [--timeout N] [--end-marker M]
```

`--accumulate-keys FILE` writes required energy keys (identifiers the energy estimator uses to look up instruction costs) to a file as a sorted comma-separated list. The file is read-merge-written on each invocation, so keys accumulate across multiple runs. Not available on `uninstrumented` commands (no energy pass).

### Package Layout

```
scripts/ckpt/
├── cli.py               # Click root group + all subcommands
├── env.py               # ProjectEnv dataclass, .env/.envrc loading, path resolution
├── errors.py            # Exception hierarchy: CkptError → ToolError, CompilationError, etc.
├── log.py               # Logging setup with module-prefixed formatting
├── toolchain.py         # Toolchain dataclass (clang/opt/llc/gcc paths), validation
├── runner.py            # subprocess wrapper: run(), StepResult, ToolError
├── tempdir.py           # @contextmanager compilation_workdir()
├── output_parser.py     # PassStatistics extraction (replaces grep|awk|sed)
├── compile/
│   ├── common.py        # compile_to_ir, annotate_tripcounts, optimize_ir,
│   │                    #   compile_to_object, assemble_and_link, collect_bb_freq
│   ├── milp.py          # MilpCompileOptions + compile_milp() (two-pass assembly + single-pass IR)
│   ├── rockclimb.py     # RockClimbCompileOptions + compile_rockclimb() (MIR pipeline)
│   ├── schematic.py     # SchematicCompileOptions + compile_schematic() (trace + insertion)
│   └── uninstrumented.py # UninstrumentedCompileOptions + compile_uninstrumented() (baseline)
├── device/
│   ├── nvm.py           # Symbol resolution, hex dump parsing
│   ├── serial.py        # UART reading
│   ├── flash.py         # flash(), read_nvm()
│   └── saleae.py        # Saleae Logic 2 automation for execution timing
├── bench/
│   ├── config.py        # Benchmark/capacitor discovery and filtering
│   ├── runner.py        # Shared benchmark matrix loop + CSV output
│   ├── milp.py          # MILP benchmark runner
│   ├── rockclimb.py     # RockClimb benchmark runner
│   ├── schematic.py     # SCHEMATIC benchmark runner (two-phase: trace once, then per-cap)
│   └── uninstrumented.py # Baseline execution time measurement (no checkpoints)
├── verify/
│   ├── common.py        # Shared verification infrastructure (verify_algorithm callback pattern)
│   ├── milp.py          # MILP semantic verification
│   ├── rockclimb.py     # RockClimb semantic verification
│   └── schematic.py     # SCHEMATIC semantic verification
└── analysis/
    ├── strip_mining.py  # Parse verbose logs for strip-mining K values
    └── plot.py          # Plot benchmark CSV results (requires `uv sync --extra plot`)
```

### Standalone Scripts

```bash
# Re-run all benchmarks (with/without device-debug + uninstrumented)
uv run python scripts/run_benchmarks.py [BENCHMARKS...]   # e.g., test aes crc rsa

# Visualize results from result/ directory
uv run --extra plot python scripts/plot_results.py [--output-dir DIR] [--normalize] [--benchmarks B...] [--metrics M...]
```

`scripts/run_benchmarks.py` runs each algorithm with and without `--device-debug`, plus uninstrumented, saving CSVs to `result/`. `scripts/plot_results.py` reads those CSVs and produces per-capacitor bar charts for 5 metrics: `region_boundaries`, `runtime_region_boundary_calls`, `execution_time`, `profiling_time`, `compilation_time`. Runtime region boundary data comes from `*-swbor.csv` (device-debug); timing data from `*-swbor-no-debug.csv`. `--normalize` normalizes to uninstrumented (when available) or MILP.

## Architecture

### Source Layout

```
passes/
├── include/
│   ├── common/        # Shared infrastructure
│   ├── estimator/     # Energy estimation interfaces
│   ├── milp/          # MILP-specific headers
│   └── schematic/     # SCHEMATIC-specific headers
├── src/
│   ├── common/        # PassRegistry, CFGAnalysis, LoopTripCount, BBFreqCollector, TripCountAnnotation
│   ├── estimator/     # IRBased, AssemblyBased estimators + factory
│   ├── milp/          # MILP pass pipeline components
│   └── schematic/     # SCHEMATIC pass pipeline components
├── bb-debuginfo/      # Separate LLVM pass: assigns BB indices as DWARF line numbers
├── bb-energy-analyzer/ # Standalone tool: computes per-BB energy from MSP430 assembly
└── runtime/           # C/assembly runtime stubs (benchmark.h, debug counters)
```

### MILP Pass Data Flow

The MILP pass pipeline has a clear staged data flow:

```
Function + LoopInfo
    │
    ├─→ EnergyEstimator (IR-based or Assembly-based)
    │       └─→ per-BB energy costs
    │
    ├─→ CFGAnalysis (blocks, edges, entry/exit, energy costs)
    │
    ├─→ StateAnalysis (all directly-accessed globals as candidates,
    │       liveness, def/use maps, access counts)
    │
    ├─→ EnergyModel (E_base, E_nvm penalties, E_save/E_restore costs,
    │       block frequencies via BlockFrequencyInfo)
    │
    ├─→ AbstractCFG (loop-collapsed graph implementing ICFGView + IStateView + IEnergyView;
    │       uses NodeId instead of string-based block names)
    │
    ├─→ CheckpointOptimizer (Gurobi MILP solver → MILPSolution)
    │
    └─→ CheckpointInstrumenter (inserts prologue/epilogue/store/restore calls)
```

### Key Abstractions

**ModelViews** (`include/milp/ModelViews.h`): Three interfaces (`ICFGView`, `IStateView`, `IEnergyView`) decouple the MILP optimizer from concrete analysis implementations. `AbstractCFG` implements all three, providing a loop-collapsed view.

**NodeMap** (`include/common/NodeMap.h`): Maps between abstract `NodeId` values and concrete `llvm::BasicBlock*` pointers. Handles both concrete nodes and summary nodes (loop-collapsed representatives).

**Context hierarchy**: `BaseContext` (estimator + CFG + LoopInfo) → `CheckpointContext` (adds StateAnalysis + EnergyModel). `ContextResult<T>` provides typed error handling for context creation.

### MILP Formulation

The optimizer uses binary and continuous decision variables:

| Variable | Type | Meaning |
|----------|------|---------|
| `isRegionStart[b]` | binary | 1 if block b starts a new checkpoint region |
| `placeInVm[b,v]` | binary | 1 if global v is placed in VM (SRAM) at block b |
| `needRestore[b,v]` | binary | 1 if global v needs restore from FRAM at block b |
| `commit[b,v]` | binary | 1 if global v is committed to FRAM at block b |
| `pending[b,v]` | binary | tracking uncommitted modifications |
| `vmPending[b,v]` | binary | VM-placed pending state |
| `energyAccumulated[b]` | continuous | accumulated energy at block b |

**Objective:** Minimize weighted sum of region starts, NVM access penalties, save/restore costs, weighted by block frequency and reboot probability.

**Constraint groups:** C1 (entry region start), C3 (VM capacity), C4 (need-restore linearization), C5 (placement propagation), C6 (pending propagation), C7 (commit model), C8 (energy init), C9 (energy propagation), C10 (buffer safety).

### RockClimb (PFI Baseline — Machine-Level)

Machine-level (post-regalloc) greedy pass in `rockclimb-backend/`. Operates on MIR after register allocation via `llc -run-pass=rockclimb`. Topological traversal inserting region boundaries when accumulated energy exceeds `E_safe = capacity - N_reg * E_restore_per_reg`. Loop headers are mandatory boundaries. Uses distributed checkpointing (saves registers at their last definition point within each region).

## Configuration

Two main config files. All fields are required (no silent defaults).

### Energy Estimator Config (`-energy-config`)

Shared by MILP and RockClimb. `estimator_type` is `"ir"` (instruction cost mapping) or `"assembly"` (pre-computed BB costs from bb-energy-analyzer).

### MILP Config (`-milp-config`)

Fields: `capacity`, `E_pro`, `E_epi`, `reg_store_energy`, `reg_restore_energy`, `nvm_access_penalty`, `mem_store_energy_per_byte`, `mem_restore_energy_per_byte`, `vm_capacity_bytes`, `q_reboot_probability`. Optional: `loop_strip_mining_enabled`.

### RockClimb Config (`-rockclimb-config`, machine pass via llc)

Fields: `capacity` (or `E_input`), `N_reg`, `reg_restore_energy`, `distributed_checkpointing`. Optional: `checkpoint_store_energy`, `add_debug_markers`.

Sample configs are in `benchmarks/` and `tests/`.

## Coding Conventions

- C++17, compiled with `-fno-exceptions -fno-rtti` to match LLVM.
- LLVM-style formatting, 4-space indentation.
- `PascalCase` for C++ classes, `snake_case` for scripts/files, `test_*.c` for tests, `*_config.json` for configs.
- Commit style: short imperative subjects (e.g., "Fix deprecated PHI insertion API in MILP instrumenter"). Scope commits to one logical change.
- All code lives in `namespace checkpoint { }`.
- **Python internal functions must not have default parameter values.** Defaults belong only in the CLI layer (`cli.py`). Internal functions (compile pipelines, benchmark runners, helpers) and dataclass fields must require all values explicitly — no `= None`, no `= ""`, no `= 0`. The only exceptions are `field(default_factory=list)` for empty collections in dataclasses.

## Python Execution

- **Always use `uv run` to execute Python commands** (e.g., `uv run pytest`, `uv run python`). Never use bare `python` or `pytest`.
- **Never manually install packages** with `uv pip install`. `uv run` auto-syncs dependencies. For optional extras use `uv run --extra test pytest ...`.

## Problem-Solving Principles

- Never propose hacky ad-hoc solutions (e.g., parsing output line-by-line with pattern matching to work around a timing issue, nested bash -c with escaped variables). Find the root cause and fix it properly.
- Prefer clean, well-understood mechanisms over clever workarounds.
- Run scripts yourself to test and debug. Do not ask the user to run scripts — if you can run them, do it yourself and continue debugging. Ask the user before making decisions, not for running commands.

## Dependencies

- **LLVM 22+** (built from source)
- **Gurobi Optimizer** (academic license available)
- **CMake 3.20+**, C++17 compiler
- **nlohmann/json** (fetched automatically by CMake)
- **Python 3.14+**, managed via **uv**
- **click>=8.1** (CLI framework, installed with `uv sync`)
- **pyserial>=3.5** (device UART communication)
- **matplotlib, numpy** (optional, for plotting: `uv sync --extra plot`)
- **logic2-automation** (optional, for Saleae timing: `uv sync --extra saleae`)
