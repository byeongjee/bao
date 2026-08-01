# AGENTS.md

This file provides guidance to coding agents (Claude Code reads it via the `.claude/CLAUDE.md` symlink) when working with code in this repository.

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

### Git Hooks

One-time setup per clone: `git config core.hooksPath .githooks`. The pre-commit hook auto-formats staged C/C++ files with clang-format and runs `ruff format` + `ruff check` on staged Python files.

clang-tidy is not part of the hook (~5s per file). Run it manually with `scripts/run_clang_tidy.sh` (changed `.cpp` files vs HEAD; `--all` for every file, parallel). Requires `passes/build/compile_commands.json`; override the binary with `CLANG_TIDY=/path/to/clang-tidy`. Checks are configured in `.clang-tidy` (exception checks are disabled because the codebase builds with `-fno-exceptions`).

## Running Passes

```bash
# Compile C to raw frontend LLVM IR (no LLVM passes run; matches the ckpt pipeline)
clang -S -emit-llvm -O3 -Xclang -disable-llvm-passes input.c -o input.ll

# MILP checkpoint insertion (-passes=checkpoint and -passes=milp are aliases)
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=checkpoint \
    -energy-config=./benchmarks/sample_energy_config_ir.json \
    -milp-config=./benchmarks/config_1uF.json \
    -S input.ll -o instrumented.ll

# RockClimb (PFI baseline — machine-level, post-regalloc via llc)
# See: ckpt compile rockclimb --help
```

### Pass Pipeline

The `checkpoint`/`milp` pass name registers a pipeline of six passes:
`LoopSimplifyPass → LCSSAPass → LoopRotatePass → IndVarSimplifyPass → LoopStripMiningPass → MILPCheckpointPass`

Loop canonicalization (LoopSimplify + LCSSA + LoopRotate + IndVarSimplify) is required before LoopStripMining can transform loops.

### CLI Options

| Option | Description |
|--------|-------------|
| `-energy-config=<path>` | Energy estimator config JSON (required for all passes) |
| `-milp-config=<path>` | MILP optimization parameters JSON (required for MILP) |
| `-rockclimb-config=<path>` | RockClimb parameters JSON (machine pass, via llc) |
| `-milp-accept-feasible` | Accept feasible (non-optimal) MILP solutions |
| `-loop-strip-mining-enabled` | Enable loop strip-mining before MILP (also settable in milp-config) |

## Test Suite

Tests use **pytest** via **uv**. Default options (`-v -n auto`) are configured in `pyproject.toml` via `addopts`.

```bash
# Run all tests
uv run pytest tests/

# Run by category (pytest marks)
uv run pytest tests/ -m milp            # MILP pass tests + scenarios
uv run pytest tests/ -m rockclimb       # RockClimb pass tests
uv run pytest tests/ -m schematic       # SCHEMATIC pass tests
uv run pytest tests/ -m vm_nvm          # VM/NVM placement tests
uv run pytest tests/ -m unit            # Pure-function unit tests (no subprocess, no device)

# Shell wrapper
./tests/run_tests.sh                # all tests
./tests/run_tests.sh --milp         # MILP only
./tests/run_tests.sh --rockclimb    # RockClimb only

# DWARF mapping validation (assembly-based workflow, separate)
./tests/dwarf-validation/validate_dwarf.sh
```

### Test file layout

| File | What it tests |
|------|---------------|
| `test_milp.py` | Basic MILP pass (success, infeasible, missing bb-freq) |
| `test_milp_vm_nvm.py` | VM/NVM placement enforcement, overflow, basic placement |
| `test_rockclimb.py` | RockClimb machine-level pass (post-regalloc) |
| `test_scenarios.py` | MILP scenarios with structural IR assertions |
| `test_schematic.py` | SCHEMATIC full trace pipeline |

Plus many `unit`-marked tests for the Python toolchain (e.g., `test_output_parser.py`, `test_bench_helpers.py`, `test_nvm_parsing.py`).

Requires `passes/build/CheckpointPass.so` to be built first. Test configs live in `tests/` and `tests/scenarios/configs/`.

## Scripts — `ckpt` Python Package

The `scripts/ckpt/` package provides the compilation, benchmarking, and device interaction toolchain as a Python CLI. Install with `uv sync` (adds `click` dependency). Run via `python -m ckpt` or the `ckpt` entry point.

### CLI Commands

```bash
# Compilation pipelines (INPUT can be a benchmark name or path to .c file)
# --csv PATH writes a one-row CSV of compile-time stats only (no device/runtime columns).
ckpt compile milp           INPUT --cap CAP [--link] [--estimator-mode assembly|ir] [--save-temps] [--halt-mode bor|lpm4|swbor] [--cpu-freq 1|8|16] [--device-debug] [--accumulate-keys FILE] [--csv CSV] ...
ckpt compile rockclimb      INPUT --cap CAP [--link] [--no-precomputed-energy] [--save-temps] [--halt-mode bor|lpm4|swbor] [--cpu-freq 1|8|16] [--device-debug] [--accumulate-keys FILE] [--csv CSV] ...
ckpt compile schematic      INPUT --cap CAP [--link] [--trace-file FILE] [--trace-only] [--save-temps] [--halt-mode bor|lpm4|swbor] [--cpu-freq 1|8|16] [--device-debug] [--accumulate-keys FILE] [--csv CSV] ...
ckpt compile uninstrumented INPUT [--link] [--save-temps] [--cpu-freq 1|8|16] [--device-debug] [--csv CSV] ...
# Explicit config paths also accepted: -e ENERGY_CONFIG -m/-c/-s ALGO_CONFIG

# Benchmark runners (compile + flash + NVM readback → CSV). The CSV output flag is
# --csv (alias -o/--output), unified with `compile`. When no MSP430 device is detected,
# bench logs a warning and degrades to compile-only: it still writes the CSV, but the
# device-only runtime columns (execution_time_us, runtime_*) are left blank.
ckpt bench milp            [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--estimator-mode] [--timeout SECONDS] [--accumulate-keys FILE] [--csv CSV]
ckpt bench rockclimb       [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--timeout SECONDS] [--accumulate-keys FILE] [--csv CSV]
ckpt bench schematic       [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--estimator-mode] [--trace-config] [--timeout SECONDS] [--accumulate-keys FILE] [--csv CSV]
ckpt bench uninstrumented  [BENCHMARKS...] [--cpu-freq] [--timeout SECONDS] [--csv CSV]

# Semantic verification defaults to --halt-mode bor, which destroys modeled volatile state
# before checkpoint recovery. Compile and bench default to swbor for continuous-power measurement.
# Multi-value --cap accepts repeats or comma lists; bare numbers get a uF suffix (--cap 5,10,50).
ckpt verify milp       [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--estimator-mode] [--cpu-freq] [--timeout SECONDS]
ckpt verify rockclimb  [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--cpu-freq] [--timeout SECONDS]
ckpt verify schematic  [BENCHMARKS...] [--cap 1uF] [--halt-mode] [--estimator-mode] [--cpu-freq] [--timeout SECONDS]
# verify all runs milp, rockclimb, schematic, schematicO3 sequentially (baseline runs once per
# benchmark, shared across caps and algorithms) and prints a combined report (-o also writes it to a file).
ckpt verify all        [BENCHMARKS...] [--cap 5,10,50] [--halt-mode] [--estimator-mode] [--cpu-freq] [--timeout SECONDS] [-o REPORT]

# Analysis
ckpt analyze strip-mining LOG_FILE [-o CSV]
ckpt analyze milp-coarse  ...    # coarse-allocation MILP analysis
ckpt analyze plot       CSV_DIR [--metric M] [--algorithms A...]

# Device interaction
ckpt device read-serial [--timeout N] [--end-marker M]
```

Additional variants: `compile|bench|verify schematicO3`, `compile|bench chunked`, and `bench uninstrumentedO0`.

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
│   ├── chunked.py       # Chunked-loop variant pipeline
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
│   ├── chunked.py       # Chunked-loop benchmark runner
│   └── uninstrumented.py # Baseline execution time measurement (no checkpoints)
├── verify/
│   ├── common.py        # Shared verification infrastructure (verify_algorithm callback pattern)
│   ├── milp.py          # MILP semantic verification
│   ├── rockclimb.py     # RockClimb semantic verification
│   └── schematic.py     # SCHEMATIC semantic verification
└── analysis/
    ├── strip_mining.py  # Parse verbose logs for strip-mining K values
    ├── milp_coarse.py   # Coarse-allocation MILP analysis
    └── plot.py          # Plot benchmark CSV results (requires `uv sync --extra plot`)
```

### Standalone Scripts

```bash
# Re-run all benchmarks (with/without device-debug + uninstrumented)
uv run python scripts/run_benchmarks.py [BENCHMARKS...]   # e.g., test aes crc rsa

# Visualize results from result/ directory
Rscript scripts/plot_results.R [--output-dir DIR] [--normalize] [--benchmarks B,...] [--metrics M,...] [--log-scale]
```

`scripts/run_benchmarks.py` runs each algorithm with and without `--device-debug`, plus uninstrumented, saving CSVs to `result/`. `scripts/plot_results.R` reads those CSVs and produces per-capacitor bar charts for 5 metrics: `region_boundaries`, `runtime_region_boundary_calls`, `execution_time`, `profiling_time`, `compilation_time`. Runtime region boundary data comes from `*-swbor.csv` (device-debug); timing data from `*-swbor-no-debug.csv`. `--normalize` normalizes to uninstrumented (when available) or MILP. `scripts/plot_results_uninstrumentedO0.R` is a variant that uses `uninstrumentedO0.csv` for the execution-time baseline.

## Architecture

### Source Layout

```
passes/
├── include/           # Headers: common/, estimator/, milp/, rockclimb/, schematic/
├── src/
│   ├── common/        # PassRegistry, CFGAnalysis, LoopTripCount, BBFreqCollector,
│   │                  #   TripCountAnnotation, EdgeSplit, BlockSplitter, RockClimbConfig
│   ├── estimator/     # IRBased, AssemblyBased estimators + factory
│   ├── milp/          # MILP pass pipeline components
│   ├── rockclimb/     # RockClimbLoopUnrollPass (IR-level preprocessing)
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
| `r[b]` | binary | 1 if block b starts a new checkpoint region |
| `m[b,v]` | binary | 1 if eligible global v is placed in VM (SRAM) at block b; fixed to 1 for ineligible objects (coarse-allocation mode uses per-global `m[v]` instead) |
| `rHat[b,v]` | continuous | need-restore indicator (`r[b] AND m[b,v]`), for eligible live-in pairs |
| `d[b,v]` | continuous | dirty indicator: v modified since the last save |
| `s[b,v]` | continuous | 1 if v is saved at region boundary b |
| `eAccum[b]` | continuous | accumulated energy at block b |

**Objective:** Minimize the frequency-weighted sum of NVM access penalties for eligible globals not placed in VM, region-start overhead (prologue + restore costs), and region-end overhead (epilogue + save costs).

**Constraint groups:** C1 (entry region start), C2 (ineligible VM placement), C3 (need-restore linearization), VM capacity, C4–C6 (dirty propagation), C8 (save at region boundary), C9 (energy init at region start), C10 (energy propagation), C11 (energy within capacity), C12–C13 (placement propagation).

### RockClimb (PFI Baseline — Machine-Level)

Machine-level (post-regalloc) greedy pass in `rockclimb-backend/`. Operates on MIR after register allocation via `llc -run-pass=rockclimb`. Topological traversal inserting region boundaries when accumulated energy exceeds `E_safe = capacity - N_reg * E_restore_per_reg`. Loop headers are mandatory boundaries. Uses distributed checkpointing (saves registers at their last definition point within each region).

## Configuration

Two main config files. The per-capacitor configs `benchmarks/config_{cap}.json` (e.g. `config_1uF.json`) are unified: shared fields at the root plus optional nested `milp`/`rockclimb`/`schematic` sections, so one file serves as the algorithm config for every pass.

### Energy Estimator Config (`-energy-config`)

Shared by MILP and RockClimb. `estimator_type` is `"ir"` (instruction cost mapping) or `"assembly"` (pre-computed BB costs from bb-energy-analyzer).

### MILP Config (`-milp-config`)

Required: `capacity`, `E_pro`, `E_epi`, `reg_store_energy`, `reg_restore_energy`, `nvm_access_penalty`, `mem_store_energy_per_byte`, `mem_restore_energy_per_byte`, `vm_capacity_bytes`. Optional: `N_reg` (default 16), and MILP-specific fields like `loop_strip_mining_enabled` (in the nested `milp` section or at root).

### RockClimb Config (`-rockclimb-config`, machine pass via llc)

Fields: `capacity` (or legacy `E_input`), `N_reg`, `reg_store_energy`, `reg_restore_energy`, `E_pro`, `E_epi`, `distributed_checkpointing` (in the nested `rockclimb` section or at root).

Sample configs are in `benchmarks/` and `tests/`.

## Coding Conventions

- C++17, compiled with `-fno-exceptions -fno-rtti` to match LLVM.
- LLVM-style formatting, 4-space indentation.
- `PascalCase` for C++ classes, `snake_case` for scripts/files, `test_*.c` for tests, `*_config.json` for configs.
- Commit style: short imperative subjects (e.g., "Fix deprecated PHI insertion API in MILP instrumenter"). Scope commits to one logical change. Keep messages concise and focused on *why* the change was made, not a restatement of *what* the diff does.
- **Never commit design documents, specs, plans, or notes.** Write them as untracked files and leave them untracked. This applies even when a skill or workflow instructs otherwise. Only code, tests, configs, and files the user explicitly asks to commit belong in git.
- All code lives in `namespace checkpoint { }`.
- **Python internal functions must not have default parameter values.** Defaults belong only in the CLI layer (`cli.py`). Internal functions (compile pipelines, benchmark runners, helpers) and dataclass fields must require all values explicitly — no `= None`, no `= ""`, no `= 0`. The only exceptions are `field(default_factory=list)` for empty collections in dataclasses.

## Python Execution

- **Always use `uv run` to execute Python commands** (e.g., `uv run pytest`, `uv run python`). Never use bare `python` or `pytest`.
- **Never manually install packages** with `uv pip install`. `uv run` auto-syncs dependencies. For optional extras use `uv run --extra test pytest ...`.
- **Always use the built-in `--timeout` option for Saleae-capable `uv run ckpt bench ...` and `uv run ckpt verify ...` executions.** Never wrap these commands with `timeout`, `gtimeout`, or another external process killer; terminating the Python process externally can bypass Saleae capture cleanup and leave Logic 2 unable to start a new session.

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
