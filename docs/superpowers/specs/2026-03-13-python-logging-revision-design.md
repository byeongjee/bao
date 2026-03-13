# Python Logging Revision

## Goal

Replace all ad-hoc `print()`/`click.echo()`/`click.secho()` output in the `ckpt` Python package with Python's `logging` stdlib, replacing `--verbose` with `--log-level`, and emitting subprocess output per-step instead of buffered at the end.

## Decisions

- **Library:** Python `logging` stdlib (no new dependencies)
- **Levels:** Standard 5 — DEBUG, INFO, WARNING, ERROR, CRITICAL
- **Default level:** INFO
- **Output destination:** All log output to stderr (keeps stdout clean for data: CSV, hex dumps)
- **Format:** Minimal for INFO+ (`INFO: message`), module-prefixed for DEBUG (`DEBUG [ckpt.compile.milp]: message`)
- **Subprocess output:** Buffered per-step, emitted immediately after each step completes via `logger.debug()`. StepResult still captures stdout/stderr for parsing.
- **Progress lines:** `[count/total]` routed through `logger.info()` (no progress bars)
- **`--log-level`:** Defined once on the root Click group, replaces all 8 per-command `--verbose` flags
- **`verbose` parameter:** Removed from all internal functions — log level controls visibility globally

## Level Assignment Rules

| Content | Level |
|---|---|
| Step announcements ("Compiling to IR...", "Flashing device...") | INFO |
| Step completion with key results (file paths, pass statistics summary, benchmark row results) | INFO |
| `[count/total]` progress lines | INFO |
| Full subprocess stdout/stderr dumps (compiler output, Gurobi log) | DEBUG |
| Multi-line pass output text | DEBUG |
| Non-fatal issues ("no benchmarks found", skipped items) | WARNING |
| Errors (compilation failure, device timeout) | ERROR |
| Raw data output to stdout (NVM hex dumps, CSV data) | Not logged — stays as `print()`/`click.echo()` to stdout |

## New Module: `scripts/ckpt/log.py`

```python
import logging
import sys


class _Formatter(logging.Formatter):
    """Minimal for INFO+, module-prefixed for DEBUG."""

    def format(self, record):
        if record.levelno <= logging.DEBUG:
            self._style._fmt = "%(levelname)s [%(name)s]: %(message)s"
        else:
            self._style._fmt = "%(levelname)s: %(message)s"
        return super().format(record)


def setup_logging(level_name: str) -> None:
    """Configure root logger. Called once from CLI entry point."""
    level = getattr(logging, level_name.upper())
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(_Formatter())
    root = logging.getLogger("ckpt")
    root.setLevel(level)
    root.addHandler(handler)
```

## CLI Changes (`cli.py`)

`--log-level` defined once on the root group:

```python
@click.group()
@click.option(
    "--log-level",
    type=click.Choice(["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"], case_sensitive=False),
    default="INFO",
    help="Set logging level.",
)
def cli(log_level):
    setup_logging(log_level)
```

All 8 per-command `--verbose` options removed. All `verbose` parameters removed from command functions and internal function calls.

## Subprocess Output Changes (`runner.py`)

`run()` emits output immediately after each subprocess completes:

```python
def run(cmd, *, step_name, ...):
    logger.info("Running %s...", step_name)
    result = subprocess.run(cmd, capture_output=True, text=True, ...)
    if result.stdout:
        logger.debug("%s stdout:\n%s", step_name, result.stdout)
    if result.stderr:
        logger.debug("%s stderr:\n%s", step_name, result.stderr)
    return StepResult(...)
```

Callers no longer concatenate output strings or conditionally print based on `verbose`.

## Files Changed

| File | Changes |
|---|---|
| `scripts/ckpt/log.py` | **New.** `_Formatter`, `setup_logging()` |
| `scripts/ckpt/cli.py` | Add `--log-level` to root group, remove 8x `--verbose`, remove `verbose` params, replace `click.echo`/`click.secho` with logger |
| `scripts/ckpt/runner.py` | Add `logger.info()` for step start, `logger.debug()` for subprocess output per-step |
| `scripts/ckpt/compile/common.py` | Remove `verbose` param, remove output concatenation, use logger |
| `scripts/ckpt/compile/milp.py` | Remove `verbose` param, remove output concatenation, use logger |
| `scripts/ckpt/compile/rockclimb.py` | Remove `verbose` param, remove output concatenation, use logger |
| `scripts/ckpt/compile/schematic.py` | Remove `verbose` param, use logger |
| `scripts/ckpt/bench/runner.py` | Remove `verbose` from `run_benchmark_matrix()`, replace `print()` with logger |
| `scripts/ckpt/bench/milp.py` | Remove `verbose` threading, use logger |
| `scripts/ckpt/bench/rockclimb.py` | Remove `verbose` threading, use logger |
| `scripts/ckpt/bench/schematic.py` | Remove `verbose` threading, use logger |
| `scripts/ckpt/verify/rockclimb.py` | Remove `verbose` threading, replace `print(..., file=sys.stderr)` with logger |
| `scripts/ckpt/analysis/plot.py` | Replace `print(..., file=sys.stderr)` with logger |
| `scripts/ckpt/device/flash.py` | Use logger for flashing progress |

**Not changed:** `output_parser.py`, `env.py`, `toolchain.py`, `tempdir.py` (no current logging).

## Example Output

### `--log-level INFO` (default)

```
INFO: Compiling input.c to IR...
INFO: Running MILP checkpoint pass...
INFO: Compiling to object...
INFO: Linking final binary...
INFO: Output: build/input.elf
```

### `--log-level DEBUG`

```
INFO: Compiling input.c to IR...
DEBUG [ckpt.runner]: clang stderr:
  <full clang output>
INFO: Running MILP checkpoint pass...
DEBUG [ckpt.runner]: opt stderr:
  <full opt output with Gurobi log>
INFO: Compiling to object...
INFO: Linking final binary...
INFO: Output: build/input.elf
```

### `--log-level WARNING`

```
(silent unless warnings or errors occur)
```
