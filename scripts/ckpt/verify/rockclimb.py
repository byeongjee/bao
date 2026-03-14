"""Semantic correctness verification for RockClimb checkpoint insertion.

Replaces verify_rockclimb.sh. For each benchmark, compiles a baseline
(no RockClimb) and a RockClimb-instrumented binary, flashes both to
hardware, reads NVM results, and compares them.
"""

from __future__ import annotations

import logging
import re
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from ..bench.config import (
    default_energy_config,
    discover_benchmarks,
    discover_capacitors,
)
from ..compile.common import (
    annotate_tripcounts,
    assemble_and_link,
    compile_runtime_c,
    compile_to_ir,
)
from ..compile.rockclimb import RockClimbCompileOptions, compile_rockclimb
from ..device.flash import read_nvm
from ..device.saleae import discover_saleae, saleae_run
from ..env import ProjectEnv
from ..output_parser import detect_infeasibility
from ..errors import DeviceError
from ..runner import CompilationError, run
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain

logger = logging.getLogger(__name__)

_CFI_RE = re.compile(r"^\s*\.cfi_")
_FLASH_TIMEOUT = 30
_AFTER_TRIGGER_SECONDS = 1.0

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
    cap_label: str
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
    cpu_freq: int,
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
        extra_defines=[f"F_CPU={cpu_freq}"],
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
        extra_defines=["DEBUG_COUNTERS", f"F_CPU={cpu_freq}"],
    )

    # Step 7: Link (no boot.S, no runtime)
    assemble_and_link(
        tc, env, [obj, debug_o], elf,
        linker_script=env.rockclimb_linker,
    )

    return elf


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def verify_rockclimb(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    halt_mode: str,
    energy_config: Path | None,
    cpu_freq: int,
) -> bool:
    """Verify semantic correctness of RockClimb checkpoint insertion.

    For each (capacitor x benchmark):
      1. Compile baseline (no RockClimb) -- same LLVM pipeline but without
         the machine pass
      2. Flash baseline, read NVM result
      3. Compile with RockClimb (using compile_rockclimb)
      4. Flash RockClimb, read NVM result
      5. Compare results

    Returns True if all benchmarks pass (or are skipped).
    """
    bench_files = discover_benchmarks(env, benchmarks)
    if not bench_files:
        logger.error("No benchmarks to verify")
        return False

    try:
        saleae_manager = discover_saleae()
    except DeviceError as exc:
        logger.error("Error: %s", exc)
        return False

    capacitors = discover_capacitors(env, "rockclimb", caps)

    if energy_config is None:
        energy_config = default_energy_config(env, "rockclimb")
    if not energy_config.is_file():
        logger.error("Energy config not found: %s", energy_config)
        return False

    results: list[_BenchResult] = []

    # Build flat list of (bench, cap) pairs for a single progress counter
    pairs = [
        (bench_path, cap)
        for bench_path in bench_files
        for cap in capacitors
    ]
    total = len(pairs)

    for idx, (bench_path, cap) in enumerate(pairs, 1):
        bench_name = bench_path.stem
        logger.info("[%d/%d] %s %s ...", idx, total, bench_name, cap.label)

        result = _verify_one(
            tc, env,
            bench_path=bench_path,
            bench_name=bench_name,
            energy_config=energy_config,
            cap_config=cap.config_path,
            cap_label=cap.label,
            saleae_manager=saleae_manager,
            halt_mode=halt_mode,
            cpu_freq=cpu_freq,
        )
        results.append(result)

    # Print summary
    _print_summary(results, halt_mode)

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
    cap_label: str,
    saleae_manager: object,
    halt_mode: str,
    cpu_freq: int,
) -> _BenchResult:
    """Run verification for a single benchmark."""
    with compilation_workdir(prefix=f"ckpt_verify_{bench_name}_") as tmp:
        # ── A: Compile baseline ──
        try:
            baseline_elf = _compile_baseline(
                tc, env, bench_path, tmp / "baseline", cpu_freq,
            )
        except (CompilationError, OSError) as exc:
            msg = f"Baseline compilation failed: {exc}"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
            )

        # ── B: Flash + read baseline ──
        try:
            saleae_run(tc, baseline_elf, saleae_manager, _FLASH_TIMEOUT, _AFTER_TRIGGER_SECONDS)
            nvm_dict = read_nvm(tc, baseline_elf, _FLASH_TIMEOUT, _NVM_SYMBOLS)
            baseline_nvm = {k: str(v) for k, v in nvm_dict.items()}
        except (DeviceError, OSError) as exc:
            msg = f"Baseline flash/read failed: {exc}"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
            )

        logger.debug("  [baseline nvm] %s", baseline_nvm)

        baseline_done = baseline_nvm.get("__nvm_done", "")
        baseline_result = baseline_nvm.get("__nvm_result", "")

        if baseline_done != "1":
            msg = f"Baseline did not complete (__nvm_done={baseline_done})"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
            )

        if not baseline_result:
            msg = "No RESULT from baseline"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
            )

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
                    link=True,
                    debug_counters=True,
                    halt_mode=halt_mode,
                    cpu_freq=cpu_freq,
                ),
            )
            compile_output = rc_result.pass_output
        except CompilationError as exc:
            compile_output = exc.result.output if hasattr(exc, "result") else str(exc)
            # Check for infeasibility in compile output
            infeasible = detect_infeasibility(compile_output)
            if infeasible:
                logger.info("  SKIP (%s)", infeasible)
                return _BenchResult(
                    bench_name, cap_label, _Status.SKIP, infeasible,
                    baseline_result=baseline_result,
                )
            msg = "RockClimb compilation failed"
            logger.debug("  %s: %s", msg, compile_output[:200])
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        logger.debug("  [rockclimb compile] %s", compile_output[:200])

        # ── D: Check for infeasibility ──
        infeasible = detect_infeasibility(compile_output)
        if infeasible:
            logger.info("  SKIP (%s)", infeasible)
            return _BenchResult(
                bench_name, cap_label, _Status.SKIP, infeasible,
                baseline_result=baseline_result,
            )

        rockclimb_elf = rc_result.elf_file
        if rockclimb_elf is None or not rockclimb_elf.exists():
            msg = "RockClimb compilation produced no ELF"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        # ── E: Flash + read RockClimb ──
        try:
            saleae_run(tc, rockclimb_elf, saleae_manager, _FLASH_TIMEOUT, _AFTER_TRIGGER_SECONDS)
            nvm_dict = read_nvm(tc, rockclimb_elf, _FLASH_TIMEOUT, _NVM_SYMBOLS)
            rockclimb_nvm = {k: str(v) for k, v in nvm_dict.items()}
        except (DeviceError, OSError) as exc:
            msg = f"RockClimb flash/read failed: {exc}"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        logger.debug("  [rockclimb nvm] %s", rockclimb_nvm)
        boundary = rockclimb_nvm.get("cnt_boundary", "?")
        save = rockclimb_nvm.get("cnt_save_reg", "?")
        restore = rockclimb_nvm.get("cnt_restore_reg", "?")
        logger.debug(
            "  [rockclimb counters] boundary=%s save_reg=%s restore_reg=%s",
            boundary, save, restore,
        )

        rc_done = rockclimb_nvm.get("__nvm_done", "")
        rc_result_val = rockclimb_nvm.get("__nvm_result", "")

        if rc_done != "1":
            msg = f"RockClimb did not complete (__nvm_done={rc_done})"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        if not rc_result_val:
            msg = "No RESULT from RockClimb"
            logger.error("  %s", msg)
            return _BenchResult(
                bench_name, cap_label, _Status.ERROR, msg,
                baseline_result=baseline_result,
            )

        # ── F: Compare results ──
        if baseline_result == rc_result_val:
            logger.info("  PASS (baseline=%s rockclimb=%s)", baseline_result, rc_result_val)
            return _BenchResult(
                bench_name, cap_label, _Status.PASS, "",
                baseline_result=baseline_result,
                rockclimb_result=rc_result_val,
            )
        else:
            logger.info("  FAIL (baseline=%s rockclimb=%s)", baseline_result, rc_result_val)
            return _BenchResult(
                bench_name, cap_label, _Status.FAIL,
                f"baseline={baseline_result} rockclimb={rc_result_val}",
                baseline_result=baseline_result,
                rockclimb_result=rc_result_val,
            )


# ---------------------------------------------------------------------------
# Summary output
# ---------------------------------------------------------------------------

def _print_summary(results: list[_BenchResult], halt_mode: str) -> None:
    """Print the verification summary table."""
    pass_count = sum(1 for r in results if r.status == _Status.PASS)
    fail_count = sum(1 for r in results if r.status == _Status.FAIL)
    skip_count = sum(1 for r in results if r.status == _Status.SKIP)
    error_count = sum(1 for r in results if r.status == _Status.ERROR)
    total = len(results)

    cap_labels = sorted({r.cap_label for r in results})

    logger.info("")
    logger.info("=== RockClimb Semantic Verification ===")
    logger.info("Halt mode: %s | Capacitors: %s", halt_mode, ", ".join(cap_labels))
    logger.info("")

    for r in results:
        cap_tag = f"[{r.cap_label}]"
        if r.status == _Status.PASS:
            line = (
                f"  {r.name:<20s} {cap_tag:<8s}"
                f" baseline={r.baseline_result or '?':<10s}"
                f" rockclimb={r.rockclimb_result or '?':<10s} PASS"
            )
        elif r.status == _Status.FAIL:
            line = (
                f"  {r.name:<20s} {cap_tag:<8s}"
                f" baseline={r.baseline_result or '?':<10s}"
                f" rockclimb={r.rockclimb_result or '?':<10s} FAIL"
            )
        elif r.status == _Status.SKIP:
            line = f"  {r.name:<20s} {cap_tag:<8s} SKIP ({r.detail})"
        else:
            line = f"  {r.name:<20s} {cap_tag:<8s} ERROR ({r.detail})"
        logger.info("%s", line)

    logger.info("")
    logger.info(
        "%d/%d PASSED, %d FAILED, %d SKIPPED, %d ERRORS",
        pass_count, total, fail_count, skip_count, error_count,
    )
