# Python Logging Revision Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace all ad-hoc print/click.echo logging with Python's `logging` stdlib, replace `--verbose` with `--log-level`, and emit subprocess output per-step.

**Architecture:** New `log.py` module configures a root `"ckpt"` logger with dual-format handler (minimal for INFO+, module-prefixed for DEBUG). All output goes to stderr. `--log-level` on the root CLI group replaces 8 per-command `--verbose` flags. `verbose` parameters removed from all internal functions; LLVM pass verbose flags tied to `pass_verbose` derived from log level.

**Tech Stack:** Python `logging` stdlib, Click CLI framework

**Spec:** `docs/superpowers/specs/2026-03-13-python-logging-revision-design.md`

---

## Chunk 1: Foundation (log.py + runner.py + cli.py root group)

### Task 1: Create `scripts/ckpt/log.py`

**Files:**
- Create: `scripts/ckpt/log.py`

- [ ] **Step 1: Create log.py**

```python
"""Logging configuration for the ckpt package."""

from __future__ import annotations

import logging
import sys

_DEBUG_FMT = logging.Formatter("%(levelname)s [%(name)s]: %(message)s")
_DEFAULT_FMT = logging.Formatter("%(levelname)s: %(message)s")


class _Formatter(logging.Formatter):
    """Minimal for INFO+, module-prefixed for DEBUG. Thread-safe."""

    def format(self, record: logging.LogRecord) -> str:
        if record.levelno <= logging.DEBUG:
            return _DEBUG_FMT.format(record)
        return _DEFAULT_FMT.format(record)


def setup_logging(level_name: str) -> None:
    """Configure the ckpt root logger. Called once from CLI entry point."""
    level = getattr(logging, level_name.upper())
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(_Formatter())
    root = logging.getLogger("ckpt")
    root.setLevel(level)
    root.addHandler(handler)
```

- [ ] **Step 2: Verify import works**

Run: `uv run python -c "from ckpt.log import setup_logging; setup_logging('INFO'); print('ok')"`
Expected: `ok` printed to stdout, no errors.

- [ ] **Step 3: Commit**

```bash
git add scripts/ckpt/log.py
git commit -m "Add ckpt.log module with dual-format logging setup"
```

---

### Task 2: Add logging to `scripts/ckpt/runner.py`

**Files:**
- Modify: `scripts/ckpt/runner.py`

- [ ] **Step 1: Add logger and per-step log emission**

Add at top of file after existing imports:

```python
import logging

logger = logging.getLogger(__name__)
```

In `run()`, add logging after the subprocess completes (before the error check). After line 57 (`input=input,`) and `elapsed_ms` calculation, before `step = StepResult(...)`:

```python
    if step_name:
        logger.info("Running %s...", step_name)
    if result.stdout:
        logger.debug("%s stdout:\n%s", step_name or "subprocess", result.stdout.rstrip())
    if result.stderr:
        logger.debug("%s stderr:\n%s", step_name or "subprocess", result.stderr.rstrip())
```

Wait — the logging should happen *after* we have the result but *before* error raising, so the output is always emitted. Place the `logger.info` call *before* `subprocess.run` and the debug output *after* the run but before the error check.

Specifically, the function body becomes:

```python
def run(
    cmd: list[str],
    *,
    check: bool = True,
    step_name: str = "",
    cwd: str | None = None,
    timeout: int = 300,
    input: str | None = None,
) -> StepResult:
    """Run a subprocess, capturing output and timing."""
    if step_name:
        logger.info("Running %s...", step_name)

    start = time.monotonic()
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        cwd=cwd,
        timeout=timeout,
        input=input,
    )
    elapsed_ms = int((time.monotonic() - start) * 1000)

    if result.stdout:
        logger.debug("%s stdout:\n%s", step_name or "subprocess", result.stdout.rstrip())
    if result.stderr:
        logger.debug("%s stderr:\n%s", step_name or "subprocess", result.stderr.rstrip())

    step = StepResult(
        returncode=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
        duration_ms=elapsed_ms,
    )

    if check and result.returncode != 0:
        raise CompilationError(step_name or " ".join(cmd[:3]), step)

    return step
```

- [ ] **Step 2: Run existing tests to verify no breakage**

Run: `uv run pytest tests/ -x -q 2>&1 | head -30`
Expected: Tests pass (or skip if hardware unavailable). No import errors.

- [ ] **Step 3: Commit**

```bash
git add scripts/ckpt/runner.py
git commit -m "Add per-step logging to subprocess runner"
```

---

### Task 3: Add `--log-level` to CLI root group

**Files:**
- Modify: `scripts/ckpt/cli.py:7-8` (imports)
- Modify: `scripts/ckpt/cli.py:53-64` (root group)

- [ ] **Step 1: Add imports**

Add after `import click` (line 11):

```python
from .log import setup_logging
```

- [ ] **Step 2: Add `--log-level` option to `main()`**

Replace the `main` function definition (lines 53-64):

```python
@click.group(cls=CkptGroup)
@click.option(
    "--log-level",
    type=click.Choice(
        ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        case_sensitive=False,
    ),
    default="INFO",
    help="Set logging level.",
)
@click.pass_context
def main(ctx: click.Context, log_level: str) -> None:
    """ckpt -- Checkpoint insertion toolchain."""
    from .env import ProjectEnv
    from .toolchain import Toolchain

    setup_logging(log_level)

    env = ProjectEnv.from_environ()
    tc = Toolchain.resolve(env)
    ctx.ensure_object(dict)
    ctx.obj["env"] = env
    ctx.obj["tc"] = tc
    ctx.obj["pass_verbose"] = log_level.upper() == "DEBUG"
```

- [ ] **Step 3: Verify CLI still works**

Run: `uv run python -m ckpt --help`
Expected: Help output shows `--log-level` option.

Run: `uv run python -m ckpt --log-level DEBUG --help`
Expected: Same help output, no errors.

- [ ] **Step 4: Commit**

```bash
git add scripts/ckpt/cli.py
git commit -m "Add --log-level option to CLI root group"
```

---

## Chunk 2: Remove verbose from compile dataclasses + all callers (atomic)

**IMPORTANT:** This chunk modifies dataclass definitions and ALL their call sites in a single commit to avoid intermediate breakage.

### Task 4: Rename/remove `verbose` on compile dataclasses and update all callers

**Files:**
- Modify: `scripts/ckpt/compile/milp.py:40,120,202`
- Modify: `scripts/ckpt/compile/rockclimb.py:33`
- Modify: `scripts/ckpt/compile/schematic.py:38`
- Modify: `scripts/ckpt/cli.py` (compile commands + imports)
- Modify: `scripts/ckpt/bench/milp.py:173`
- Modify: `scripts/ckpt/bench/rockclimb.py:145`
- Modify: `scripts/ckpt/bench/schematic.py:194,254`
- Modify: `scripts/ckpt/verify/rockclimb.py:289`

- [ ] **Step 1: Add logger import at top of `cli.py`**

After the `from .log import setup_logging` added in Task 3, add:

```python
import logging

logger = logging.getLogger(__name__)
```

- [ ] **Step 2: Update `compile/milp.py` — rename `verbose` to `pass_verbose`**

On `MilpCompileOptions` (line 40): change `verbose: bool` to `pass_verbose: bool`.
Line 120: `if opts.verbose:` → `if opts.pass_verbose:`
Line 202: `if opts.verbose:` → `if opts.pass_verbose:`

- [ ] **Step 3: Update `compile/rockclimb.py` — remove `verbose` field**

On `RockClimbCompileOptions` (line 33): delete `verbose: bool`. Field is unused by `compile_rockclimb()`.

- [ ] **Step 4: Update `compile/schematic.py` — remove `verbose` field**

On `SchematicCompileOptions` (line 38): delete `verbose: bool`. Field is unused by `compile_schematic()`.

- [ ] **Step 5: Update CLI `compile_milp_cmd`**

Remove `@click.option("--verbose", ...)` decorator (line 88).
Remove `verbose: bool` parameter (line 110).
In `MilpCompileOptions(...)` constructor (line 136): change `verbose=verbose` to `pass_verbose=ctx.obj["pass_verbose"]`.
Replace output lines (146-151):

```python
    logger.info("Object: %s", result.object_file)
    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)
```

- [ ] **Step 6: Update CLI `compile_rockclimb_cmd`**

Remove `@click.option("--verbose", ...)` decorator (line 160).
Remove `verbose: bool` parameter (line 182).
In `RockClimbCompileOptions(...)` constructor: remove `verbose=verbose` entirely.
Replace output lines (211-215):

```python
    logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    logger.debug("Pass output:\n%s", result.pass_output)
```

- [ ] **Step 7: Update CLI `compile_schematic_cmd`**

Remove `@click.option("--verbose", ...)` decorator (line 224).
Remove `verbose: bool` parameter (line 255).
In `SchematicCompileOptions(...)` constructor: remove `verbose=verbose` entirely.
Replace output lines (301-310):

```python
    if result.object_file:
        logger.info("Object: %s", result.object_file)
    if result.assembly_file:
        logger.info("Assembly: %s", result.assembly_file)
    if result.elf_file:
        logger.info("ELF: %s", result.elf_file)
    if result.trace_file:
        logger.info("Trace: %s", result.trace_file)
    if result.pass_output:
        logger.debug("Pass output:\n%s", result.pass_output)
```

- [ ] **Step 8: Update CLI `compile_run_cmd`**

Remove `@click.option("--verbose", ...)` decorator (line 330).
Remove `verbose: bool` parameter (line 360).

For each mode's dataclass construction:
- MILP (line 393): change `verbose=verbose` to `pass_verbose=ctx.obj["pass_verbose"]`
- RockClimb (line 425): remove `verbose=verbose`
- SCHEMATIC (line 453): remove `verbose=verbose`

Replace `click.echo(...)` output with logger calls:
- `click.echo(f"MILP compiled: ...")` → `logger.info("MILP compiled: %s", result.object_file)`
- `if verbose: click.echo(result.pass_output)` → `logger.debug("Pass output:\n%s", result.pass_output)`
- Same pattern for rockclimb and schematic blocks.
- `click.echo("Mode 'none': ...")` → `logger.info("Mode 'none': no checkpoint insertion.")`
- `click.echo(f"ELF: {elf}")` → `logger.info("ELF: %s", elf)`
- `click.echo("Flashing and reading output...")` → `logger.info("Flashing and reading output...")`
- `click.echo(output_text)` at line 485 — keep as `click.echo(output_text)` (raw device data to stdout, not a log message).

- [ ] **Step 9: Update bench runner compile closures**

`bench/milp.py` line 173: change `verbose=True` to `pass_verbose=True`.
`bench/rockclimb.py` line 145: remove `verbose=True,` entirely.
`bench/schematic.py` line 194: remove `verbose=verbose,` from trace `SchematicCompileOptions`.
`bench/schematic.py` line 254: remove `verbose=verbose,` from per-cap `SchematicCompileOptions`.

- [ ] **Step 10: Update `verify/rockclimb.py` dataclass construction**

Line 289: remove `verbose=verbose,` from `RockClimbCompileOptions(...)`.

- [ ] **Step 11: Run tests**

Run: `uv run pytest tests/test_milp.py tests/test_scenarios.py -x -q 2>&1 | head -30`
Expected: All tests pass.

- [ ] **Step 12: Commit all together**

```bash
git add scripts/ckpt/compile/milp.py scripts/ckpt/compile/rockclimb.py \
       scripts/ckpt/compile/schematic.py scripts/ckpt/cli.py \
       scripts/ckpt/bench/milp.py scripts/ckpt/bench/rockclimb.py \
       scripts/ckpt/bench/schematic.py scripts/ckpt/verify/rockclimb.py
git commit -m "Remove --verbose from compile commands, rename to pass_verbose for LLVM flags"
```

---

## Chunk 3: Benchmark modules

### Task 5: Update `scripts/ckpt/bench/runner.py`

**Files:**
- Modify: `scripts/ckpt/bench/runner.py:151-166` (function signature)
- Modify: `scripts/ckpt/bench/runner.py:213,268,276-277,284,296,307,341-350,377-475` (print calls)

- [ ] **Step 1: Add logger**

Add at top after imports:

```python
import logging

logger = logging.getLogger(__name__)
```

- [ ] **Step 2: Remove `verbose` from `run_benchmark_matrix` signature**

In the function signature (line 161), remove:
```python
    verbose: bool,
```

- [ ] **Step 3: Replace all `print()` calls with logger**

Apply these replacements throughout `run_benchmark_matrix`:

| Line | Old | New |
|------|-----|-----|
| 213 | `print(f"[{count}/{total}] Running {row_name} ...")` | `logger.info("[%d/%d] Running %s ...", count, total, row_name)` |
| 268 | `print(f"  DEVICE ERROR: {exc}")` | `logger.error("  DEVICE ERROR: %s", exc)` |
| 276-277 | `if verbose:\n    print(full_output)` | `logger.debug("Full output:\n%s", full_output)` |
| 284 | `print(f"  INFEASIBLE ({json_reason})")` | `logger.warning("  INFEASIBLE (%s)", json_reason)` |
| 296 | `print(f"  INFEASIBLE ({infeasible_reason})")` | `logger.warning("  INFEASIBLE (%s)", infeasible_reason)` |
| 307 | `print("  FAILED (compilation error)")` | `logger.error("  FAILED (compilation error)")` |
| 341 | `print()` | `logger.info("")` |
| 342 | `print("--- Energy parameters ---")` | `logger.info("--- Energy parameters ---")` |
| 343 | `print(f"  Required ...")` | `logger.info("  Required (%d keys): %s", len(all_required_keys), ", ".join(sorted(all_required_keys)))` |
| 344 | `print(f"  Missing ...")` | `logger.info("  Missing  (%d keys): %s", len(all_missing_keys), ", ".join(sorted(all_missing_keys)))` |
| 347-350 | Final summary prints | `logger.info("")`; `logger.info("==========================================")` etc. |

- [ ] **Step 4: Replace `print()` calls in `print_benchmark_summary`**

Replace all `print(...)` calls in `print_benchmark_summary` (lines 377-474) with `logger.info(...)`. The function uses formatted strings — convert each `print(f"...")` to `logger.info(...)`.

- [ ] **Step 5: Commit**

```bash
git add scripts/ckpt/bench/runner.py
git commit -m "Replace print/verbose with logger in bench/runner.py"
```

---

### Task 6: Update `scripts/ckpt/bench/milp.py`

**Files:**
- Modify: `scripts/ckpt/bench/milp.py:97-197`

Note: The compile option `verbose=True` → `pass_verbose=True` change at line 173 was already done in Task 4 Step 9.

- [ ] **Step 1: Remove `verbose` parameter from `run_milp_benchmarks`**

Remove `verbose: bool,` from the function signature (line 106).
Remove `verbose` from the docstring.

- [ ] **Step 2: Remove `verbose` from `run_benchmark_matrix` call**

In the call on line 193: remove `verbose=verbose,`.

- [ ] **Step 3: Commit**

```bash
git add scripts/ckpt/bench/milp.py
git commit -m "Remove verbose threading from MILP bench runner"
```

---

### Task 7: Update `scripts/ckpt/bench/rockclimb.py`

**Files:**
- Modify: `scripts/ckpt/bench/rockclimb.py:79-167`

Note: The compile option `verbose=True` removal at line 145 was already done in Task 4 Step 9.

- [ ] **Step 1: Remove `verbose` parameter from `run_rockclimb_benchmarks`**

Remove `verbose: bool,` from the function signature (line 88).
Remove `verbose` from the docstring.

- [ ] **Step 2: Remove `verbose` from `run_benchmark_matrix` call**

In the call on line 163: remove `verbose=verbose,`.

- [ ] **Step 3: Commit**

```bash
git add scripts/ckpt/bench/rockclimb.py
git commit -m "Remove verbose threading from RockClimb bench runner"
```

---

### Task 8: Update `scripts/ckpt/bench/schematic.py`

**Files:**
- Modify: `scripts/ckpt/bench/schematic.py:110-296`

Note: The `verbose=verbose` removals from `SchematicCompileOptions` at lines 194 and 254 were already done in Task 4 Step 9.

- [ ] **Step 1: Add logger**

Add at top after imports:

```python
import logging

logger = logging.getLogger(__name__)
```

- [ ] **Step 2: Remove `verbose` parameter from `run_schematic_benchmarks`**

Remove `verbose: bool,` from the function signature (line 119).
Remove `verbose` from the docstring.

- [ ] **Step 3: Update `pre_benchmark` closure — replace prints with logger**

Replace the print/verbose usage in `pre_benchmark` (lines 177-239):

| Line | Old | New |
|------|-----|-----|
| 181 | `print()` | `logger.info("")` |
| 182 | `print(f"--- Collecting trace for {bench_name} ---")` | `logger.info("--- Collecting trace for %s ---", bench_name)` |
| 208-209 | `if verbose:\n    print(trace_result.pass_output)` | `logger.debug("Trace output:\n%s", trace_result.pass_output)` |
| 230 | `print(f"  Trace collected for ...")` | `logger.info("  Trace collected for %s (profiling: %dms)", bench_name, profiling_time_ms)` |
| 233 | `print(f"  FAILED: trace collection for {bench_name}")` | `logger.error("  FAILED: trace collection for %s", bench_name)` |
| 234-238 | `if verbose: ...` block | `if isinstance(exc, CompilationError) and exc.result: logger.debug("%s", exc.result.output[-500:])` then `else: logger.debug("%s", exc)` |

- [ ] **Step 4: Remove `verbose` from `run_benchmark_matrix` call**

In the call on line 293: remove `verbose=verbose,`.

- [ ] **Step 5: Commit**

```bash
git add scripts/ckpt/bench/schematic.py
git commit -m "Remove verbose threading from SCHEMATIC bench runner"
```

---

### Task 9: Update CLI bench commands

**Files:**
- Modify: `scripts/ckpt/cli.py:497-652` (bench commands)

- [ ] **Step 1: Update `bench_milp_cmd`**

Remove `@click.option("-v", "--verbose", ...)` (line 502).
Remove `verbose: bool` parameter (line 528).
Remove `verbose=verbose,` from `run_milp_benchmarks(...)` call (line 544).

- [ ] **Step 2: Update `bench_rockclimb_cmd`**

Remove `@click.option("-v", "--verbose", ...)` (line 555).
Remove `verbose: bool` parameter (line 575).
Remove `verbose=verbose,` from `run_rockclimb_benchmarks(...)` call (line 590).

- [ ] **Step 3: Update `bench_schematic_cmd`**

Remove `@click.option("-v", "--verbose", ...)` (line 600).
Remove `verbose: bool` parameter (line 630).
Remove `verbose=verbose,` from `run_schematic_benchmarks(...)` call (line 648).

- [ ] **Step 4: Verify CLI help**

Run: `uv run python -m ckpt bench milp --help`
Expected: No `--verbose` option listed. `--log-level` available from root.

- [ ] **Step 5: Commit**

```bash
git add scripts/ckpt/cli.py
git commit -m "Remove --verbose from bench commands"
```

---

## Chunk 4: Verify + analysis modules

### Task 10: Update `scripts/ckpt/verify/rockclimb.py`

**Files:**
- Modify: `scripts/ckpt/verify/rockclimb.py`

Note: The `verbose=verbose` removal from `RockClimbCompileOptions` at line 289 was already done in Task 4 Step 10.

- [ ] **Step 1: Add logger, remove `import sys`**

Remove `import sys` (line 11) — after migration, `sys.stderr` is no longer used.
Add after existing imports:

```python
import logging

logger = logging.getLogger(__name__)
```

- [ ] **Step 2: Remove `verbose` from `verify_rockclimb` signature**

Line 161: remove `verbose: bool,` from the parameter list.

- [ ] **Step 3: Replace prints in `verify_rockclimb`**

| Line | Old | New |
|------|-----|-----|
| 182 | `print("Error: No benchmarks to verify", file=sys.stderr)` | `logger.error("No benchmarks to verify")` |
| 189-192 | `print(f"Error: RockClimb config not found: ...", file=sys.stderr)` | `logger.error("RockClimb config not found: %s", rockclimb_config)` |
| 198 | `print(f"Error: Energy config not found: ...", file=sys.stderr)` | `logger.error("Energy config not found: %s", energy_config)` |
| 206 | `print(f"[{idx}/{total}] {bench_name} ...")` | `logger.info("[%d/%d] %s ...", idx, total, bench_name)` |

- [ ] **Step 4: Remove `verbose` from `_verify_one` and replace all conditional prints**

Remove `verbose: bool,` from `_verify_one` signature (line 238).
Remove `verbose=verbose,` from the call site (line 215).

Replace all `if verbose:` conditional prints with unconditional `logger.debug()`:

| Line | Old | New |
|------|-----|-----|
| 250-251 | `if verbose:\n    print(f"  {msg}")` | `logger.debug("  %s", msg)` |
| 259-260 | `if verbose:\n    print(f"  {msg}")` | `logger.debug("  %s", msg)` |
| 263-264 | `if verbose:\n    print(f"  [baseline nvm] {baseline_nvm}")` | `logger.debug("  [baseline nvm] %s", baseline_nvm)` |

Lines 271, 276: unconditional `print(f"  ERROR: {msg}")` → `logger.error("  %s", msg)`

| Line | Old | New |
|------|-----|-----|
| 301 | `print(f"  SKIP ({infeasible})")` | `logger.info("  SKIP (%s)", infeasible)` |
| 307-308 | `if verbose:\n    print(f"  {msg}: {compile_output[:200]}")` | `logger.debug("  %s: %s", msg, compile_output[:200])` |
| 314-315 | `if verbose:\n    print(f"  [rockclimb compile] ...")` | `logger.debug("  [rockclimb compile] %s", compile_output[:200])` |
| 320 | `print(f"  SKIP ({infeasible})")` | `logger.info("  SKIP (%s)", infeasible)` |
| 329 | `print(f"  ERROR: {msg}")` | `logger.error("  %s", msg)` |
| 340-341 | `if verbose:\n    print(f"  {msg}")` | `logger.debug("  %s", msg)` |
| 347-355 | `if verbose:` block with nvm/counter prints | `logger.debug("  [rockclimb nvm] %s", rockclimb_nvm)` etc. — all unconditional `logger.debug()` |
| 362 | `print(f"  ERROR: {msg}")` | `logger.error("  %s", msg)` |
| 370 | `print(f"  ERROR: {msg}")` | `logger.error("  %s", msg)` |
| 378-381 | `print(f"  PASS ...")` | `logger.info("  PASS (baseline=%s rockclimb=%s)", baseline_result, rc_result_val)` |
| 388-391 | `print(f"  FAIL ...")` | `logger.info("  FAIL (baseline=%s rockclimb=%s)", baseline_result, rc_result_val)` |

- [ ] **Step 5: Replace prints in `_print_summary`**

Replace all `print(...)` in `_print_summary` (lines 404-438) with `logger.info(...)`.

- [ ] **Step 6: Commit**

```bash
git add scripts/ckpt/verify/rockclimb.py
git commit -m "Replace print/verbose with logger in verify/rockclimb.py"
```

---

### Task 11: Update CLI verify command

**Files:**
- Modify: `scripts/ckpt/cli.py:672,699,711` (verify rockclimb command)

- [ ] **Step 1: Update `verify_rockclimb_cmd`**

Remove `@click.option("-v", "--verbose", ...)` (line 672).
Remove `verbose: bool` parameter (line 699).
Remove `verbose=verbose,` from `verify_rockclimb(...)` call (line 711).

- [ ] **Step 2: Commit**

```bash
git add scripts/ckpt/cli.py
git commit -m "Remove --verbose from verify command"
```

---

### Task 12: Update `scripts/ckpt/analysis/plot.py`

**Files:**
- Modify: `scripts/ckpt/analysis/plot.py`

- [ ] **Step 1: Add logger, remove `import sys`**

Remove `import sys` (line 10) — after migration, `sys.stderr` is no longer used.
Add after existing imports:

```python
import logging

logger = logging.getLogger(__name__)
```

- [ ] **Step 2: Replace print calls**

| Line | Old | New |
|------|-----|-----|
| 126-129 | `print(f"Warning: {filepath} not found, skipping {alg_key}", file=sys.stderr)` | `logger.warning("%s not found, skipping %s", filepath, alg_key)` |
| 224-228 | `print(f"Error: baseline algorithm '{normalize}' has no data.", file=sys.stderr)` | `logger.error("Baseline algorithm '%s' has no data.", normalize)` |
| 235 | `print("No data to plot.", file=sys.stderr)` | `logger.warning("No data to plot.")` |
| 296 | `print(f"Saved to {output_file}")` | `logger.info("Saved to %s", output_file)` |

- [ ] **Step 3: Commit**

```bash
git add scripts/ckpt/analysis/plot.py
git commit -m "Replace print with logger in analysis/plot.py"
```

---

### Task 13: Update `scripts/ckpt/analysis/strip_mining.py`

**Files:**
- Modify: `scripts/ckpt/analysis/strip_mining.py:182`

- [ ] **Step 1: Add logger**

Add at top after imports:

```python
import logging

logger = logging.getLogger(__name__)
```

- [ ] **Step 2: Replace print**

Line 182: `print(f"Wrote {len(rows)} rows to {output_path}")` → `logger.info("Wrote %d rows to %s", len(rows), output_path)`

- [ ] **Step 3: Commit**

```bash
git add scripts/ckpt/analysis/strip_mining.py
git commit -m "Replace print with logger in analysis/strip_mining.py"
```

---

## Chunk 5: Device commands + final cleanup

### Task 14: Update CLI device commands

**Files:**
- Modify: `scripts/ckpt/cli.py:800-833` (device commands)

- [ ] **Step 1: Update device output**

`device_read_nvm_cmd` (line 817-818): Keep `click.echo(f"{sym}={val}")` — this is raw data output to stdout, not a log message.

`device_read_serial_cmd` (line 833): Keep `click.echo(output)` — this is raw data output to stdout.

No changes needed for device commands. The data output correctly stays on stdout.

- [ ] **Step 2: Verify no remaining click.echo calls that should be logger**

Search for remaining `click.echo` and `click.secho` in `cli.py`. The only ones that should remain are:
- `click.secho(f"Error: {exc}", fg="red", err=True)` in `CkptGroup.invoke()` (line 45) — intentionally kept
- `click.echo(f"{sym}={val}")` in `device_read_nvm_cmd` — raw data
- `click.echo(output)` in `device_read_serial_cmd` — raw data

- [ ] **Step 3: Commit (if any changes)**

Only commit if changes were made.

---

### Task 15: Final integration test

- [ ] **Step 1: Run full test suite**

Run: `uv run pytest tests/ -x -q 2>&1 | tail -20`
Expected: All tests pass.

- [ ] **Step 2: Verify no remaining `verbose` parameters**

Run: `grep -rn "verbose" scripts/ckpt/ --include="*.py" | grep -v __pycache__ | grep -v "^.*#" | grep -v '"""'`
Expected: No hits from function signatures or variable usage. Comments mentioning "verbose" (e.g., `rockclimb.py:285` "non-verbose mode", `strip_mining.py` docstrings) are expected and harmless.

- [ ] **Step 3: Manual smoke test**

Run: `uv run python -m ckpt --help`
Expected: `--log-level` visible, no `--verbose` anywhere.

Run: `uv run python -m ckpt compile milp --help`
Expected: No `--verbose` option.

- [ ] **Step 4: Final commit (if any fixups needed)**

```bash
git add -A scripts/ckpt/
git commit -m "Final cleanup: remove any remaining verbose references"
```
