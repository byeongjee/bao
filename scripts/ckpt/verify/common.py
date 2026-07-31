"""Shared verification infrastructure for all checkpoint algorithms.

Provides the common verify loop: for each (benchmark x capacitor),
compile an uninstrumented baseline and an instrumented binary, flash
both to hardware, read NVM results, and compare.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from saleae.automation import Manager

from ..compile.uninstrumented import (
    UninstrumentedCompileOptions,
    compile_uninstrumented,
)
from ..device.flash import read_nvm
from ..device.saleae import discover_saleae, saleae_run
from ..env import ProjectEnv
from ..errors import DeviceError
from ..output_parser import detect_infeasibility
from ..runner import CompilationError, StepResult
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain

logger = logging.getLogger(__name__)

_FLASH_TIMEOUT = 30
_AFTER_TRIGGER_SECONDS = 1.0
_POST_CAPTURE_SETTLE_SECONDS = 2.0

# Only need result + done from the baseline (no counter symbols).
_BASELINE_NVM_SYMBOLS = [
    "__nvm_done",
    "__nvm_result",
]


class Status(Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"
    ERROR = "ERROR"


@dataclass
class BenchResult:
    name: str
    cap_label: str
    status: Status
    detail: str
    baseline_result: str | None = None
    algorithm_result: str | None = None


@dataclass
class InstrumentedOutput:
    """Result of an instrumented compilation for verification."""

    compile_output: str
    elf_file: Path | None


# Callback: compile the instrumented binary for one (benchmark, capacitor).
# Signature: (tc, env, bench_path, workdir, cap_config, halt_mode, cpu_freq)
CompileInstrumentedFn = Callable[
    [Toolchain, ProjectEnv, Path, Path, Path, str, int],
    InstrumentedOutput,
]


def verify_algorithm(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    algorithm: str,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    halt_mode: str,
    cpu_freq: int,
    capture_timeout_seconds: float,
    nvm_symbols: list[str],
    compile_instrumented: CompileInstrumentedFn,
) -> bool:
    """Run semantic verification for a checkpoint algorithm.

    For each (benchmark x capacitor):
      1. Compile uninstrumented baseline (shared across algorithms)
      2. Flash baseline, read __nvm_result
      3. Compile instrumented via *compile_instrumented*
      4. Flash instrumented, read __nvm_result
      5. Compare results

    Returns True if all pass or skip (no failures or errors).
    """
    from ..bench.config import discover_benchmarks, discover_capacitors

    bench_files = discover_benchmarks(env, benchmarks)
    if not bench_files:
        logger.error("No benchmarks to verify")
        return False

    try:
        saleae_manager = discover_saleae()
    except DeviceError as exc:
        logger.error("Error: %s", exc)
        return False
    try:
        capacitors = discover_capacitors(env, algorithm, caps)

        results: list[BenchResult] = []

        pairs = [(bench_path, cap) for bench_path in bench_files for cap in capacitors]
        total = len(pairs)

        for idx, (bench_path, cap) in enumerate(pairs, 1):
            bench_name = bench_path.stem
            logger.info("[%d/%d] %s %s ...", idx, total, bench_name, cap.label)

            result = _verify_one(
                tc,
                env,
                algorithm=algorithm,
                bench_path=bench_path,
                bench_name=bench_name,
                cap_config=cap.config_path,
                cap_label=cap.label,
                saleae_manager=saleae_manager,
                halt_mode=halt_mode,
                cpu_freq=cpu_freq,
                capture_timeout_seconds=capture_timeout_seconds,
                nvm_symbols=nvm_symbols,
                compile_instrumented=compile_instrumented,
            )
            results.append(result)

        _print_summary(results, algorithm, halt_mode)

        return not any(r.status in (Status.FAIL, Status.ERROR) for r in results)
    finally:
        saleae_manager.close()


def _compile_baseline(
    tc: Toolchain,
    env: ProjectEnv,
    bench_path: Path,
    output: Path,
    cpu_freq: int,
) -> Path:
    """Compile an uninstrumented baseline ELF with debug counters."""
    result = compile_uninstrumented(
        tc,
        env,
        UninstrumentedCompileOptions(
            input_c=bench_path,
            output=output,
            device_debug=True,
            cpu_freq=cpu_freq,
            opt_level=3,
            clang_opt_level=3,
            link=True,
        ),
    )

    elf = result.elf_file
    if elf is None or not elf.exists():
        raise CompilationError(
            "baseline-link",
            StepResult(
                returncode=1,
                stdout="",
                stderr="No ELF produced",
                duration_ms=0,
            ),
        )
    return elf


def _verify_one(
    tc: Toolchain,
    env: ProjectEnv,
    *,
    algorithm: str,
    bench_path: Path,
    bench_name: str,
    cap_config: Path,
    cap_label: str,
    saleae_manager: Manager,
    halt_mode: str,
    cpu_freq: int,
    capture_timeout_seconds: float,
    nvm_symbols: list[str],
    compile_instrumented: CompileInstrumentedFn,
) -> BenchResult:
    """Run verification for a single (benchmark, capacitor) pair."""
    with compilation_workdir(prefix=f"ckpt_verify_{bench_name}_") as tmp:
        # -- A: Compile + flash baseline --
        try:
            baseline_elf = _compile_baseline(
                tc,
                env,
                bench_path,
                tmp / "baseline",
                cpu_freq,
            )
        except (CompilationError, OSError) as exc:
            msg = f"Baseline compilation failed: {exc}"
            logger.error("  %s", msg)
            return BenchResult(bench_name, cap_label, Status.ERROR, msg)

        try:
            saleae_run(
                baseline_elf,
                saleae_manager,
                _FLASH_TIMEOUT,
                _AFTER_TRIGGER_SECONDS,
                capture_timeout_seconds,
            )
            # The stop pulse fires before debug_exit() stores the result and halts.
            # Reconnecting with mspdebug too early resets the target and can restart
            # the benchmark before __nvm_done/__nvm_result are final.
            time.sleep(_POST_CAPTURE_SETTLE_SECONDS)
            baseline_nvm = read_nvm(
                tc, baseline_elf, _FLASH_TIMEOUT, _BASELINE_NVM_SYMBOLS
            )
        except (DeviceError, OSError) as exc:
            msg = f"Baseline flash/read failed: {exc}"
            logger.error("  %s", msg)
            return BenchResult(bench_name, cap_label, Status.ERROR, msg)

        baseline_done = str(baseline_nvm.get("__nvm_done", ""))
        baseline_result = str(baseline_nvm.get("__nvm_result", ""))
        logger.debug(
            "  [baseline nvm] done=%s result=%s", baseline_done, baseline_result
        )

        if baseline_done != "1":
            msg = f"Baseline did not complete (__nvm_done={baseline_done})"
            logger.error("  %s", msg)
            return BenchResult(bench_name, cap_label, Status.ERROR, msg)

        if not baseline_result:
            msg = "No RESULT from baseline"
            logger.error("  %s", msg)
            return BenchResult(bench_name, cap_label, Status.ERROR, msg)

        # -- B: Compile instrumented --
        try:
            inst = compile_instrumented(
                tc,
                env,
                bench_path,
                tmp,
                cap_config,
                halt_mode,
                cpu_freq,
            )
            compile_output = inst.compile_output
        except CompilationError as exc:
            compile_output = exc.pass_output or (
                exc.result.output if exc.result else str(exc)
            )
            infeasible = detect_infeasibility(compile_output)
            if infeasible:
                logger.warning("  SKIP (%s)", infeasible)
                return BenchResult(
                    bench_name,
                    cap_label,
                    Status.SKIP,
                    infeasible,
                    baseline_result=baseline_result,
                )
            msg = f"{algorithm} compilation failed"
            logger.debug("  %s: %s", msg, compile_output[:200])
            return BenchResult(
                bench_name,
                cap_label,
                Status.ERROR,
                msg,
                baseline_result=baseline_result,
            )

        # -- C: Check infeasibility --
        infeasible = detect_infeasibility(compile_output)
        if infeasible:
            logger.warning("  SKIP (%s)", infeasible)
            return BenchResult(
                bench_name,
                cap_label,
                Status.SKIP,
                infeasible,
                baseline_result=baseline_result,
            )

        inst_elf = inst.elf_file
        if inst_elf is None or not inst_elf.exists():
            msg = f"{algorithm} compilation produced no ELF"
            logger.error("  %s", msg)
            return BenchResult(
                bench_name,
                cap_label,
                Status.ERROR,
                msg,
                baseline_result=baseline_result,
            )

        # -- D: Flash + read instrumented --
        try:
            saleae_run(
                inst_elf,
                saleae_manager,
                _FLASH_TIMEOUT,
                _AFTER_TRIGGER_SECONDS,
                capture_timeout_seconds,
            )
            time.sleep(_POST_CAPTURE_SETTLE_SECONDS)
            inst_nvm = read_nvm(tc, inst_elf, _FLASH_TIMEOUT, nvm_symbols)
        except (DeviceError, OSError) as exc:
            msg = f"{algorithm} flash/read failed: {exc}"
            logger.error("  %s", msg)
            return BenchResult(
                bench_name,
                cap_label,
                Status.ERROR,
                msg,
                baseline_result=baseline_result,
            )

        inst_done = str(inst_nvm.get("__nvm_done", ""))
        inst_result_val = str(inst_nvm.get("__nvm_result", ""))
        logger.debug("  [%s nvm] %s", algorithm, inst_nvm)

        if inst_done != "1":
            msg = f"{algorithm} did not complete (__nvm_done={inst_done})"
            logger.error("  %s", msg)
            return BenchResult(
                bench_name,
                cap_label,
                Status.ERROR,
                msg,
                baseline_result=baseline_result,
            )

        if not inst_result_val:
            msg = f"No RESULT from {algorithm}"
            logger.error("  %s", msg)
            return BenchResult(
                bench_name,
                cap_label,
                Status.ERROR,
                msg,
                baseline_result=baseline_result,
            )

        # -- E: Compare --
        if baseline_result == inst_result_val:
            logger.info(
                "  PASS (baseline=%s %s=%s)",
                baseline_result,
                algorithm,
                inst_result_val,
            )
            return BenchResult(
                bench_name,
                cap_label,
                Status.PASS,
                "",
                baseline_result=baseline_result,
                algorithm_result=inst_result_val,
            )
        else:
            logger.error(
                "  FAIL (baseline=%s %s=%s)",
                baseline_result,
                algorithm,
                inst_result_val,
            )
            return BenchResult(
                bench_name,
                cap_label,
                Status.FAIL,
                f"baseline={baseline_result} {algorithm}={inst_result_val}",
                baseline_result=baseline_result,
                algorithm_result=inst_result_val,
            )


def _print_summary(
    results: list[BenchResult],
    algorithm: str,
    halt_mode: str,
) -> None:
    """Print the verification summary table."""
    pass_count = sum(1 for r in results if r.status == Status.PASS)
    fail_count = sum(1 for r in results if r.status == Status.FAIL)
    skip_count = sum(1 for r in results if r.status == Status.SKIP)
    error_count = sum(1 for r in results if r.status == Status.ERROR)
    total = len(results)

    cap_labels = sorted({r.cap_label for r in results})

    logger.info("")
    logger.info("=== %s Semantic Verification ===", algorithm.upper())
    logger.info("Halt mode: %s | Capacitors: %s", halt_mode, ", ".join(cap_labels))
    logger.info("")

    for r in results:
        cap_tag = f"[{r.cap_label}]"
        if r.status == Status.PASS:
            line = (
                f"  {r.name:<20s} {cap_tag:<8s}"
                f" baseline={r.baseline_result or '?':<10s}"
                f" {algorithm}={r.algorithm_result or '?':<10s} PASS"
            )
        elif r.status == Status.FAIL:
            line = (
                f"  {r.name:<20s} {cap_tag:<8s}"
                f" baseline={r.baseline_result or '?':<10s}"
                f" {algorithm}={r.algorithm_result or '?':<10s} FAIL"
            )
        elif r.status == Status.SKIP:
            line = f"  {r.name:<20s} {cap_tag:<8s} SKIP ({r.detail})"
        else:
            line = f"  {r.name:<20s} {cap_tag:<8s} ERROR ({r.detail})"
        logger.info("%s", line)

    logger.info("")
    logger.info(
        "%d/%d PASSED, %d FAILED, %d SKIPPED, %d ERRORS",
        pass_count,
        total,
        fail_count,
        skip_count,
        error_count,
    )
