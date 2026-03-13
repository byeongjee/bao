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
- **LLVM pass verbose flags:** In `compile/milp.py`, `verbose` controls whether extra LLVM flags (`-loop-strip-mining-verbose`, `-abstract-cfg-verbose`) are passed to `opt`. These flags are now tied to the log level: pass them when `log_level == DEBUG`. The compile options dataclasses replace `verbose: bool` with `pass_verbose: bool`, set by the CLI layer based on `log_level == "DEBUG"`.
- **CLI error handler:** `CkptGroup.invoke()` keeps its `click.secho(..., fg="red", err=True)` — it's the top-level catch-all in the CLI layer, not an internal function.
- **`warnings.warn()`:** Calls in `bench/config.py` stay as `warnings.warn()` — they are standard library warnings with different semantics (user-filterable via `-W`), not log messages.
- **Colored output:** The `click.secho(..., fg="red")` color in the error handler is preserved (see above). All other output migrates to uncolored logging. Color is not added to the formatter.

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


_DEBUG_FMT = logging.Formatter("%(levelname)s [%(name)s]: %(message)s")
_DEFAULT_FMT = logging.Formatter("%(levelname)s: %(message)s")


class _Formatter(logging.Formatter):
    """Minimal for INFO+, module-prefixed for DEBUG. Thread-safe."""

    def format(self, record):
        if record.levelno <= logging.DEBUG:
            return _DEBUG_FMT.format(record)
        return _DEFAULT_FMT.format(record)


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

The root group function is `main` (defined with `@click.group(cls=CkptGroup)`). Add `--log-level` there:

```python
@click.group(cls=CkptGroup)
@click.option(
    "--log-level",
    type=click.Choice(["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"], case_sensitive=False),
    default="INFO",
    help="Set logging level.",
)
@click.pass_context
def main(ctx, log_level, ...):
    setup_logging(log_level)
    # pass_verbose derived from log level for LLVM pass flags
    ctx.ensure_object(dict)
    ctx.obj["pass_verbose"] = log_level.upper() == "DEBUG"
```

All 8 per-command `--verbose` options removed. All `verbose` parameters removed from command functions and internal function calls. Compile commands read `ctx.obj["pass_verbose"]` and pass it to compile option dataclasses.

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
| `scripts/ckpt/cli.py` | Add `--log-level` to `main()` root group, remove 8x `--verbose`, remove `verbose` params, pass `ctx.obj["pass_verbose"]` to compile option dataclasses, replace `click.echo`/`click.secho` with logger (keep `CkptGroup.invoke()` error handler as `click.secho`) |
| `scripts/ckpt/runner.py` | Add `logger.info()` for step start, `logger.debug()` for subprocess output per-step |
| `scripts/ckpt/compile/milp.py` | Replace `verbose: bool` with `pass_verbose: bool` on `MilpCompileOptions`, use `pass_verbose` to control LLVM flags (`-loop-strip-mining-verbose`, `-abstract-cfg-verbose`), remove output concatenation, use logger |
| `scripts/ckpt/compile/rockclimb.py` | Remove `verbose` from `RockClimbCompileOptions` (unused in compile function), remove output concatenation, use logger |
| `scripts/ckpt/compile/schematic.py` | Remove `verbose` param, use logger |
| `scripts/ckpt/bench/runner.py` | Remove `verbose` from `run_benchmark_matrix()`, replace ~18 `print()` calls: progress `[count/total]` -> `logger.info()`, `INFEASIBLE` -> `logger.warning()`, `DEVICE ERROR` / `FAILED` -> `logger.error()`, energy params summary -> `logger.info()`, final summary -> `logger.info()`, per-benchmark detail -> `logger.info()`, verbose full output dump -> `logger.debug()` |
| `scripts/ckpt/bench/milp.py` | Update compile option construction from `verbose=True` to `pass_verbose=True`. Remove `verbose` parameter from `run_milp_benchmarks()`. Remove `verbose` from `run_benchmark_matrix()` call. Use logger. |
| `scripts/ckpt/bench/rockclimb.py` | Update compile option construction from `verbose=True` to `pass_verbose=True`. Remove `verbose` parameter from `run_rockclimb_benchmarks()`. Remove `verbose` from `run_benchmark_matrix()` call. Use logger. |
| `scripts/ckpt/bench/schematic.py` | Update compile option construction to use `pass_verbose`. Remove `verbose` parameter from `run_schematic_benchmarks()`. Replace ~7 `print()` calls in trace collection: step announcements -> `logger.info()`, trace output -> `logger.debug()`, errors -> `logger.error()` |
| `scripts/ckpt/verify/rockclimb.py` | Remove `verbose` parameter from `verify_rockclimb()` and `_verify_one()`. In `_verify_one()`, all `if verbose:` conditional output becomes unconditional `logger.debug()`. Replace ~25 `print()` calls: progress `[count/total]` -> `logger.info()`, PASS/FAIL/SKIP status -> `logger.info()`, error conditions -> `logger.error()`, summary table -> `logger.info()`, `print(..., file=sys.stderr)` -> `logger.error()` |
| `scripts/ckpt/analysis/plot.py` | Replace `print(..., file=sys.stderr)` with `logger.error()` / `logger.warning()`. Replace `print(f"Saved to ...")` (stdout) with `logger.info()`. |
| `scripts/ckpt/analysis/strip_mining.py` | Replace `print(f"Wrote {len(rows)} rows to ...")` with `logger.info()` |
**Not changed:** `output_parser.py` (pure data parsing), `env.py`, `toolchain.py`, `tempdir.py` (no current logging), `compile/common.py` (no `verbose` param or print calls), `bench/config.py` (`warnings.warn()` stays as-is), `errors.py` (`CkptGroup.invoke()` error handler stays as `click.secho`), `device/flash.py` (no print/echo calls), `device/serial.py`, `device/nvm.py`, `device/saleae.py`.

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
