"""Semantic correctness verification for RockClimb checkpoint insertion.

Replaces verify_rockclimb.sh. For each benchmark, compiles a baseline
(no RockClimb) and a RockClimb-instrumented binary, flashes both to
hardware, reads NVM results, and compares them.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from ..compile.common import (
    annotate_tripcounts,
    assemble_and_link,
    compile_runtime_c,
    compile_to_ir,
)
from ..compile.rockclimb import RockClimbCompileOptions, compile_rockclimb
from ..env import ProjectEnv
from ..output_parser import detect_infeasibility
from ..errors import DeviceError
from ..runner import CompilationError, run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain

_CFI_RE = re.compile(r"^\s*\.cfi_")

# NVM symbols read from the device for both baseline and RockClimb binaries.
_NVM_SYMBOLS = [
    "__nvm_done",
    "__nvm_result",
    "cnt_boundary",
    "cnt_save_reg",
    "cnt_restore_reg",
]


class _Status(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    ERROR = "ERROR"


@dataclass
class _BenchResult:
    name: str
    status: _Status
    detail: str
    baseline_result: str | None = None
    rockclimb_result: str | None = None


# ---------------------------------------------------------------------------
# Baseline compilation (no RockClimb machine pass)
# ---------------------------------------------------------------------------

def _compile_baseline(
    tc: Toolchain,
    env: ProjectEnv,
    input_c: Path,
    prefix: Path,
) -> Path:
    """Compile a baseline ELF (same LLVM pipeline, no machine pass).

    Steps:
      1. clang -S -emit-llvm -O2 --target=msp430 with -DDEBUG_COUNTERS
      2. opt -passes=tripcount-annotation
      3. llc -march=msp430 (straight, no machine pass)
      4. Strip .cfi_* directives
      5. gcc assemble
      6. gcc link with rockclimb_debug_counters.c and rockclimb linker
         (no boot.S, no runtime -- baseline has no checkpoints)

    Returns the path to the linked ELF.
    """
    raw_ll = Path(f"{prefix}.raw.ll")
    annotated_ll = Path(f"{prefix}.ll")
    raw_s = Path(f"{prefix}.raw.s")
    clean_s = Path(f"{prefix}.s")
    obj = Path(f"{prefix}.o")
    debug_o = Path(f"{prefix}.debug_counters.o")
    elf = Path(f"{prefix}.elf")

    # Step 1: C -> LLVM IR
    compile_to_ir(
        tc, env, input_c, raw_ll,
        clang_opt_level=2,
        debug=False,
        debug_counters=True,
        extra_includes=[str(env.project_dir / "passes" / "runtime")],
    )

    # Step 2: Tripcount annotation
    annotate_tripcounts(tc, env, raw_ll, annotated_ll)

    # Step 3: llc to assembly (no machine pass)
    run(
        [tc.llc, "-march=msp430", str(annotated_ll), "-o", str(raw_s)],
        step_name="llc-baseline",
    )

    # Step 4: Strip .cfi_* directives
    lines = raw_s.read_text().splitlines(keepends=True)
    clean_s.write_text("".join(line for line in lines if not _CFI_RE.match(line)))

    # Step 5: Assemble
    run(
        [
            tc.gcc,
            f"-mmcu={env.device}",
            "-msmall",
            "-c", str(clean_s),
            "-o", str(obj),
        ],
        step_name="gcc-assemble-baseline",
    )

    # Step 6: Compile debug counters
    compile_runtime_c(
        tc, env, env.rockclimb_debug_counters, debug_o,
        extra_defines=["DEBUG_COUNTERS"],
    )

    # Step 7: Link (no boot.S, no runtime)
    assemble_and_link(
        tc, env, [obj, debug_o], elf,
        linker_script=env.rockclimb_linker,
    )

    return elf


# ---------------------------------------------------------------------------
# NVM reading helpers
# ---------------------------------------------------------------------------

def _read_nvm(tc: Toolchain, elf: Path, symbols: list[str], timeout: int) -> dict[str, str]:
    """Flash, run, and read NVM symbols. Returns key=value dict."""
    from ..device.flash import flash_run_and_read

    nvm_dict = flash_run_and_read(tc, elf, timeout, symbols)
    return {k: str(v) for k, v in nvm_dict.items()}


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def verify_rockclimb(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None = None,
    cap_size: str = "1uF",
    timeout: int = 30,
    verbose: bool,
    halt_mode: str,
    energy_config: Path | None = None,
    rockclimb_config: Path | None = None,
) -> bool:
    """Verify semantic correctness of RockClimb checkpoint insertion.

    For each benchmark:
      1. Compile baseline (no RockClimb) -- same LLVM pipeline but without
         the machine pass
      2. Flash baseline, read NVM result
      3. Compile with RockClimb (using compile_rockclimb)
      4. Flash RockClimb, read NVM result
      5. Compare results

    Returns True if all benchmarks pass (or are skipped).
    """
    from ..bench.config import discover_benchmarks

    bench_files = discover_benchmarks(env, benchmarks)
    if not bench_files:
        print("Error: No benchmarks to verify", file=sys.stderr)
        return False

    # Resolve config paths (use defaults if not provided)
    if rockclimb_config is None:
        rockclimb_config = env.project_dir / "benchmarks" / f"config_{cap_size}.json"
    if not rockclimb_config.is_file():
        print(
            f"Error: RockClimb config not found: {rockclimb_config}",
            file=sys.stderr,
        )
        return False

    if energy_config is None:
        energy_config = env.project_dir / "benchmarks" / "sample_assembly_energy_params.json"
    if not energy_config.is_file():
        print(f"Error: Energy config not found: {energy_config}", file=sys.stderr)
        return False

    results: list[_BenchResult] = []
    total = len(bench_files)

    for idx, bench_path in enumerate(bench_files, 1):
        bench_name = bench_path.stem
        print(f"[{idx}/{total}] {bench_name} ...")

        result = _verify_one(
            tc, env,
            bench_path=bench_path,
            bench_name=bench_name,
            energy_config=energy_config,
            cap_config=rockclimb_config,
            timeout=timeout,
            verbose=verbose,
            halt_mode=halt_mode,
        )
        results.append(result)

    # Print summary
    _print_summary(results, cap_size)

    # Return True only if no failures or errors
    return not any(
        r.status in (_Status.FAIL, _Status.ERROR) for r in results
    )


def _verify_one(
    tc: Toolchain,
    env: ProjectEnv,
    *,
    bench_path: Path,
    bench_name: str,
    energy_config: Path,
    cap_config: Path,
    timeout: int,
    verbose: bool,
    halt_mode: str,
) -> _BenchResult:
    """Run verification for a single benchmark."""
    with compilation_workdir(prefix=f"ckpt_verify_{bench_name}_") as tmp:
        # ── A: Compile baseline ──
        try:
            baseline_elf = _compile_baseline(
                tc, env, bench_path, tmp / "baseline",
            )
        except (CompilationError, OSError) as exc:
            msg = f"Baseline compilation failed: {exc}"
            if verbose:
                print(f"  {msg}")
            return _BenchResult(bench_name, _Status.ERROR, msg)

        # ── B: Flash + read baseline ──
        try:
            baseline_nvm = _read_nvm(tc, baseline_elf, _NVM_SYMBOLS, timeout)
        except (DeviceError, OSError) as exc:
            msg = f"Baseline flash/read failed: {exc}"
            if verbose:
                print(f"  {msg}")
            return _BenchResult(bench_name, _Status.ERROR, msg)

        if verbose:
            print(f"  [baseline nvm] {baseline_nvm}")

        baseline_done = baseline_nvm.get("__nvm_done", "")
        baseline_result = baseline_nvm.get("__nvm_result", "")

        if baseline_done != "1":
            msg = f"Baseline did not complete (__nvm_done={baseline_done})"
            print(f"  ERROR: {msg}")
            return _BenchResult(bench_name, _Status.ERROR, msg)

        if not baseline_result:
            msg = "No RESULT from baseline"
            print(f"  ERROR: {msg}")
            return _BenchResult(bench_name, _Status.ERROR, msg)

        # ── C: Compile with RockClimb ──
        try:
            rc_result = compile_rockclimb(
                tc, env,
                RockClimbCompileOptions(
                    input_c=bench_path,
                    energy_config=energy_config,
                    rockclimb_config=cap_config,
                    output=tmp / "rockclimb",
                    precomputed_energy=True,
                    verbose=verbose,
                    link=True,
                    debug_counters=True,
                    halt_mode=halt_mode,
                ),
            )
            compile_output = rc_result.pass_output
        except CompilationError as exc:
            compile_output = exc.result.output if hasattr(exc, "result") else str(exc)
            # Check for infeasibility in compile output
            infeasible = detect_infeasibility(compile_output)
            if infeasible:
                print(f"  SKIP ({infeasible})")
                return _BenchResult(
                    bench_name, _Status.SKIP, infeasible,
                    baseline_result=baseline_result,
                )
            msg = "RockClimb compilation failed"
            if verbose:
                print(f"  {msg}: {compile_output[:200]}")
            return _BenchResult(
                bench_name, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        if verbose:
            print(f"  [rockclimb compile] {compile_output[:200]}")

        # ── D: Check for infeasibility ──
        infeasible = detect_infeasibility(compile_output)
        if infeasible:
            print(f"  SKIP ({infeasible})")
            return _BenchResult(
                bench_name, _Status.SKIP, infeasible,
                baseline_result=baseline_result,
            )

        rockclimb_elf = rc_result.elf_file
        if rockclimb_elf is None or not rockclimb_elf.exists():
            msg = "RockClimb compilation produced no ELF"
            print(f"  ERROR: {msg}")
            return _BenchResult(
                bench_name, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        # ── E: Flash + read RockClimb ──
        try:
            rockclimb_nvm = _read_nvm(tc, rockclimb_elf, _NVM_SYMBOLS, timeout)
        except (DeviceError, OSError) as exc:
            msg = f"RockClimb flash/read failed: {exc}"
            if verbose:
                print(f"  {msg}")
            return _BenchResult(
                bench_name, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        if verbose:
            print(f"  [rockclimb nvm] {rockclimb_nvm}")
            boundary = rockclimb_nvm.get("cnt_boundary", "?")
            save = rockclimb_nvm.get("cnt_save_reg", "?")
            restore = rockclimb_nvm.get("cnt_restore_reg", "?")
            print(
                f"  [rockclimb counters] boundary={boundary}"
                f" save_reg={save} restore_reg={restore}"
            )

        rc_done = rockclimb_nvm.get("__nvm_done", "")
        rc_result_val = rockclimb_nvm.get("__nvm_result", "")

        if rc_done != "1":
            msg = f"RockClimb did not complete (__nvm_done={rc_done})"
            print(f"  ERROR: {msg}")
            return _BenchResult(
                bench_name, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        if not rc_result_val:
            msg = "No RESULT from RockClimb"
            print(f"  ERROR: {msg}")
            return _BenchResult(
                bench_name, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        # ── F: Compare results ──
        if baseline_result == rc_result_val:
            print(
                f"  PASS (baseline={baseline_result}"
                f" rockclimb={rc_result_val})"
            )
            return _BenchResult(
                bench_name, _Status.PASS, "",
                baseline_result=baseline_result,
                rockclimb_result=rc_result_val,
            )
        else:
            print(
                f"  FAIL (baseline={baseline_result}"
                f" rockclimb={rc_result_val})"
            )
            return _BenchResult(
                bench_name, _Status.FAIL,
                f"baseline={baseline_result} rockclimb={rc_result_val}",
                baseline_result=baseline_result,
                rockclimb_result=rc_result_val,
            )


# ---------------------------------------------------------------------------
# Summary output
# ---------------------------------------------------------------------------

def _print_summary(results: list[_BenchResult], cap_size: str) -> None:
    """Print the verification summary table."""
    pass_count = sum(1 for r in results if r.status == _Status.PASS)
    fail_count = sum(1 for r in results if r.status == _Status.FAIL)
    skip_count = sum(1 for r in results if r.status == _Status.SKIP)
    error_count = sum(1 for r in results if r.status == _Status.ERROR)
    total = len(results)

    print()
    print("=== RockClimb Semantic Verification ===")
    print(f"Halt mode: debug | Capacitor: {cap_size}")
    print()

    for r in results:
        if r.status == _Status.PASS:
            line = (
                f"  {r.name:<20s} baseline={r.baseline_result or '?':<10s}"
                f" rockclimb={r.rockclimb_result or '?':<10s} PASS"
            )
        elif r.status == _Status.FAIL:
            line = (
                f"  {r.name:<20s} baseline={r.baseline_result or '?':<10s}"
                f" rockclimb={r.rockclimb_result or '?':<10s} FAIL"
            )
        elif r.status == _Status.SKIP:
            line = f"  {r.name:<20s} SKIP ({r.detail})"
        else:
            line = f"  {r.name:<20s} ERROR ({r.detail})"
        print(line)

    print()
    print(
        f"{pass_count}/{total} PASSED, {fail_count} FAILED,"
        f" {skip_count} SKIPPED, {error_count} ERRORS"
    )
