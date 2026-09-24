# BAO: MILP-Based Checkpoint Insertion for Intermittent Computing

Bao is a compiler toolchain that inserts checkpoints automatically into any
program compiled to LLVM IR. Its core is an LLVM pass that formulates
checkpoint placement as a **Mixed-Integer Linear Program** (solved with
Gurobi): minimize expected runtime overhead subject to the constraint that
no execution path can exceed the energy available in the capacitor. It also
includes the two baselines used for comparison, **RockClimb** and
**SCHEMATIC**.

## Quick Start

### Requirements

Software:
- Docker
- Python 3.14+ with [uv](https://docs.astral.sh/uv/)
- mspdebug
- Saleae Logic 2, with the automation server enabled (docs/saleae.md)
- Otii 3 desktop app, for `otii_server`
- R (optional), to plot the results

Licenses:
- Gurobi WLS license
- Otii Automation Toolbox license

Hardware (wiring in docs/intermittent.md):
- TI MSP430FR5994 LaunchPad
- Saleae Logic analyzer
- Otii Ace Pro
- Qoitech Switchboard
- 560 Ω resistor and a schottky diode

### Setup

```bash
docker build -t bao .
export GRB_LICENSE_FILE=/path/to/gurobi.lic
export OTII_SERVER_BIN=/path/to/otii_server
uv sync --extra saleae --extra otii
```

### Evaluation Scripts

Each experiment in the paper is one command. The results go to `results/`,
overwriting the results used in the paper (use `-d DIR` to write elsewhere),
together with a `numbers.txt` that lists every number quoted in the paper.
`--skip-existing` resumes an interrupted run. To also plot the results,
install R and drop `--no-plot`.

Every experiment runs with the board, the Otii Ace Pro, the Switchboard and
the Saleae connected as in docs/intermittent.md. The Otii main output is
wired in one of two ways:

**Direct wiring** (Otii main output straight to the board supply rail):

| Experiment (paper section) | Command |
|---|---|
| Execution time and boundary hits (§6.2, §6.6) | `uv run ckpt bench all --cap 5,10,50 --no-plot` |
| RockClimb with unroll factor 64 on crc (§6.2) | `uv run ckpt bench rockclimb-unroll64` |
| Execution time for the intermittent-power decomposition (§6.3) | `uv run ckpt bench decomposition` |
| Loop chunking overhead (§6.4) | `uv run ckpt bench chunking-overhead` |
| Trip-count annotations (§6.5) | `uv run ckpt bench trip-count` |
| Constant memory allocation (§6.6.2) | `uv run ckpt bench milp-coarse` |
| Inlining (§6.1, compile only, no board needed) | `uv run ckpt analyze inlining` |

**Harvester wiring** (through the 560 Ω resistor and the schottky diode):

| Experiment (paper section) | Command |
|---|---|
| Execution under intermittent power (§6.3) | `uv run ckpt intermittent all --no-plot` |

`ckpt bench decomposition` writes to `results/intermittent`, where
`ckpt intermittent all` reads it for its decomposition tables.

### Testing without Hardware

Without any hardware, you can still compile by adding `--save-build DIR`.
A Gurobi license is still required.

```bash
uv run ckpt bench all --cap 5,10,50 --save-build build
```

## Other

### Tests

```bash
tests/run_tests.sh
```

### License

MIT License

## Development

After changing the source, rebuild the image (`docker build -t bao .`).
Input and output paths given to `ckpt` must be under the current directory.
