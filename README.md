# MILP-Based Checkpoint Insertion for Intermittent Computing

Batteryless devices powered by energy harvesting compute on small bursts of
energy buffered in a capacitor, and lose power whenever the buffer runs out.
To make progress across power failures, programs must checkpoint their state
to non-volatile memory — but checkpoints are expensive, so *where* the
compiler places them determines most of the runtime overhead.

This repository is a compiler toolchain that inserts checkpoints
automatically into any program compiled to LLVM IR (the bundled pipelines
and benchmarks use C, the most common case). Its core is an LLVM pass that
formulates checkpoint placement as a
**Mixed-Integer Linear Program** (solved with Gurobi):
minimize expected runtime overhead — checkpoint calls, state save/restore,
and volatile-vs-non-volatile data placement — subject to the constraint that
no execution path can exceed the energy available in the capacitor.

The repository also includes two baselines used for comparison:

- **RockClimb (PFI)** — a greedy machine-level pass that runs after register
  allocation and inserts region boundaries when accumulated energy exceeds a
  safe threshold.
- **SCHEMATIC** — a trace-based checkpoint insertion pipeline.

The target platform is the TI **MSP430FR5994**, an FRAM-based
microcontroller commonly used in intermittent-computing research.

## How It Works

For each function, the MILP pass:

1. estimates per-basic-block energy costs (from an IR-level cost model, or
   from measured MSP430 assembly costs),
2. builds a loop-aware control-flow graph annotated with block frequencies
   measured by an instrumented profiling run,
3. solves a MILP that chooses checkpoint region boundaries and
   volatile/non-volatile placement of program state, subject to the
   capacitor's energy capacity, and
4. instruments the IR with checkpoint and state save/restore calls.

## Requirements

- **LLVM** built from source. Last verified against LLVM 23.0.0git, commit
  [`384cecd5b201`](https://github.com/llvm/llvm-project/commit/384cecd5b20107e1453eaee008e8f4fd22b42c9e).
  All plugins must be built against the same LLVM tree as the `opt`/`llc`
  you run; after updating LLVM, rebuild with
  `cmake --build passes/build --clean-first`.
- **Gurobi Optimizer** (free academic licenses available)
- **CMake 3.20+** and a C++17 compiler
- **Python 3.14+** with [uv](https://docs.astral.sh/uv/)
- **MSP430 GCC toolchain** (`msp430-elf-gcc`) — only needed for linking
  binaries and running on hardware

## Building

```bash
export LLVM_DIR=/path/to/llvm-project/build
export GUROBI_HOME=/path/to/gurobi   # e.g., /Library/gurobi1300/macos_universal2

cd passes && mkdir -p build && cd build
cmake .. -DLLVM_DIR=$LLVM_DIR/lib/cmake/llvm
make
```

## Quick Start

The `ckpt` CLI drives the full pipeline (profiling, energy estimation,
MILP solving, instrumentation):

```bash
uv sync

# Compile a benchmark with checkpoints sized for a 1 µF capacitor.
# INPUT is a benchmark name from benchmarks/intermittent/ or a path to a .c file.
uv run ckpt compile milp test --cap 1uF

# Compile + flash + measure on an attached MSP430 board. Running the bench
# itself requires a Saleae Logic analyzer — see docs/saleae.md for setup.
# Degrades to compile-only if no device is detected:
uv run ckpt bench milp test --cap 1uF --csv results.csv
```

Other pipelines follow the same shape: `ckpt compile rockclimb ...`,
`ckpt compile schematic ...`, `ckpt compile uninstrumented ...`. See
`uv run ckpt --help`.

## Running the Pass Directly

The `ckpt` CLI automates the steps below. The MILP pass reads block
frequencies from a JSON file produced by the repository's own profiling
instrumentation (the `bb-freq-collect` pass), passed via `-bb-freq-file`:

```bash
# -O0 emits IR whose loops still mirror the source, so trip-count markers can
# be annotated first; optimization happens later in opt (-passes=default<O3>).
# -disable-O0-optnone lets passes run on -O0 IR.
clang -S -emit-llvm -O0 -Xclang -disable-O0-optnone input.c -o input.ll

# Instrument for BB-frequency profiling, run natively → bb_freq.json
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=bb-freq-collect -S input.ll -o input_freq.ll
clang input_freq.ll passes/runtime/bb_freq_runtime.c -o freq_run
./freq_run    # writes bb_freq.json

# Insert checkpoints
opt -load-pass-plugin=./passes/build/CheckpointPass.so \
    -passes=milp \
    -energy-config=./benchmarks/sample_energy_config_ir.json \
    -milp-config=./benchmarks/config_1uF.json \
    -bb-freq-file=bb_freq.json \
    -S input.ll -o instrumented.ll
```

## Tests

```bash
uv run pytest tests/
```

Requires `passes/build/CheckpointPass.so` to be built first.

## More Documentation

Architecture, the MILP formulation, configuration reference, the full CLI,
and contributor setup (git hooks, clang-tidy) are documented in
[`AGENTS.md`](AGENTS.md).

## License

MIT License
