"""Shared verification infrastructure for all checkpoint algorithms.

Provides the common verify loop: for each benchmark, compile and run an
uninstrumented baseline once (its result is capacitor-independent), then
for each (capacitor x algorithm), compile an instrumented binary, flash
it to hardware, read NVM results, and compare against the baseline.
"""

from __future__ import annotations

import logging
import time
from collections.abc import Callable
from contextlib import closing
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from saleae.automation import Manager

from ..bench.runner import (
    AFTER_TRIGGER_SECONDS,
    FLASH_TIMEOUT,
    POST_CAPTURE_SETTLE_SECONDS,
)
from ..compile.uninstrumented import (
    UninstrumentedCompileOptions,
    compile_uninstrumented,
)
from ..device.flash import check_region_violation, raise_if_region_violation, read_nvm
from ..device.otii import debugger_connection
from ..device.saleae import discover_saleae, saleae_run
from ..env import ProjectEnv
from ..errors import CompilationError, ConfigError, DeviceError, RegionViolationError
from ..output_parser import detect_infeasibility
from ..runner import StepResult
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain

logger = logging.getLogger(__name__)

# Only need result + done from the baseline (no counter symbols).
_BASELINE_NVM_SYMBOLS = [
    "__nvm_done",
    "__nvm_result",
]

# Defines applied to every verification build. Benchmarks may use VERIFY_BUILD
# to shrink their workload; baseline and instrumented binaries must be compiled
# with the same defines or their results diverge.
VERIFY_DEFINES = ("VERIFY_BUILD",)


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
    baseline_result: str | None
    algorithm_result: str | None


@dataclass
class InstrumentedOutput:
    """Result of an instrumented compilation for verification."""

    compile_output: str
    elf_file: Path | None


# Callback: compile the instrumented binary for one (benchmark, capacitor).
# Signature: (tc, env, bench_path, workdir, cap_config, halt_mode, cpu_freq,
#             extra_defines)
CompileInstrumentedFn = Callable[
    [Toolchain, ProjectEnv, Path, Path, Path, str, int, list[str]],
    InstrumentedOutput,
]


@dataclass
class AlgorithmSpec:
    """One checkpoint algorithm to verify."""

    name: str
    nvm_symbols: list[str]
    compile_instrumented: CompileInstrumentedFn


def verify_algorithms(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    algorithms: list[AlgorithmSpec],
    benchmarks: list[str] | None,
    caps: list[str] | None,
    halt_mode: str,
    cpu_freq: int,
    capture_timeout_seconds: float,
) -> dict[str, list[BenchResult]]:
    """Run semantic verification for one or more checkpoint algorithms.

    For each benchmark:
      1. Compile uninstrumented baseline, flash it, read __nvm_result
         (once per benchmark -- the baseline is capacitor-independent)
    then for each (capacitor x algorithm):
      2. Compile instrumented via the algorithm's *compile_instrumented*
      3. Flash instrumented, read __nvm_result
      4. Compare against the baseline result

    Returns per-algorithm result lists, keyed by algorithm name.
    """
    from ..bench.config import discover_benchmarks, discover_capacitors

    bench_files = discover_benchmarks(env, benchmarks)
    if not bench_files:
        raise ConfigError("No benchmarks to verify")

    # The relays must close before the Saleae/device probing below: with the
    # intermittent-power rig wired up they carry the ez-FET's SBW and 3V3
    # lines to the target.
    with debugger_connection(), closing(discover_saleae()) as saleae_manager:
        capacitors = discover_capacitors(env, algorithms[0].name, caps)

        results: dict[str, list[BenchResult]] = {spec.name: [] for spec in algorithms}
        total = len(bench_files) * len(capacitors) * len(algorithms)
        idx = 0

        for bench_path in bench_files:
            bench_name = bench_path.stem
            with compilation_workdir(prefix=f"ckpt_verify_{bench_name}_") as tmp:
                logger.info("%s: baseline ...", bench_name)
                baseline_result, baseline_error = _run_baseline(
                    tc,
                    env,
                    bench_path=bench_path,
                    workdir=tmp,
                    cpu_freq=cpu_freq,
                    saleae_manager=saleae_manager,
                    capture_timeout_seconds=capture_timeout_seconds,
                )

                for cap in capacitors:
                    for spec in algorithms:
                        idx += 1
                        logger.info(
                            "[%d/%d] %s %s %s ...",
                            idx,
                            total,
                            spec.name,
                            bench_name,
                            cap.label,
                        )

                        if baseline_error is not None:
                            result = BenchResult(
                                bench_name,
                                cap.label,
                                Status.ERROR,
                                baseline_error,
                                baseline_result=None,
                                algorithm_result=None,
                            )
                        else:
                            assert baseline_result is not None
                            workdir = tmp / f"{spec.name}_{cap.label}"
                            workdir.mkdir()
                            result = _verify_instrumented(
                                tc,
                                env,
                                spec=spec,
                                bench_path=bench_path,
                                bench_name=bench_name,
                                cap_config=cap.config_path,
                                cap_label=cap.label,
                                workdir=workdir,
                                saleae_manager=saleae_manager,
                                halt_mode=halt_mode,
                                cpu_freq=cpu_freq,
                                capture_timeout_seconds=capture_timeout_seconds,
                                baseline_result=baseline_result,
                            )
                        results[spec.name].append(result)

        for spec in algorithms:
            _print_summary(results[spec.name], spec.name, halt_mode)

        return results


def all_ok(results: list[BenchResult]) -> bool:
    """True if no result failed or errored (passes and skips are fine)."""
    return not any(r.status in (Status.FAIL, Status.ERROR) for r in results)


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
            extra_defines=list(VERIFY_DEFINES),
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


def _run_baseline(
    tc: Toolchain,
    env: ProjectEnv,
    *,
    bench_path: Path,
    workdir: Path,
    cpu_freq: int,
    saleae_manager: Manager,
    capture_timeout_seconds: float,
) -> tuple[str | None, str | None]:
    """Compile, flash, and run the uninstrumented baseline for one benchmark.

    Returns ``(baseline_result, None)`` on success or ``(None, error)``.
    """
    try:
        baseline_elf = _compile_baseline(
            tc,
            env,
            bench_path,
            workdir / "baseline",
            cpu_freq,
        )
    except (CompilationError, OSError) as exc:
        msg = f"Baseline compilation failed: {exc}"
        logger.error("  %s", msg)
        return None, msg

    try:
        saleae_run(
            baseline_elf,
            saleae_manager,
            FLASH_TIMEOUT,
            AFTER_TRIGGER_SECONDS,
            capture_timeout_seconds,
        )
        # The stop pulse fires before debug_exit() stores the result and halts.
        # Reconnecting with mspdebug too early resets the target and can restart
        # the benchmark before __nvm_done/__nvm_result are final.
        time.sleep(POST_CAPTURE_SETTLE_SECONDS)
        baseline_nvm = read_nvm(tc, baseline_elf, FLASH_TIMEOUT, _BASELINE_NVM_SYMBOLS)
    except (DeviceError, OSError) as exc:
        msg = f"Baseline flash/read failed: {exc}"
        logger.error("  %s", msg)
        return None, msg

    baseline_done = str(baseline_nvm.get("__nvm_done", ""))
    baseline_result = str(baseline_nvm.get("__nvm_result", ""))
    logger.debug("  [baseline nvm] done=%s result=%s", baseline_done, baseline_result)

    if baseline_done != "1":
        msg = f"Baseline did not complete (__nvm_done={baseline_done})"
        logger.error("  %s", msg)
        return None, msg

    if not baseline_result:
        msg = "No RESULT from baseline"
        logger.error("  %s", msg)
        return None, msg

    return baseline_result, None


def _verify_instrumented(
    tc: Toolchain,
    env: ProjectEnv,
    *,
    spec: AlgorithmSpec,
    bench_path: Path,
    bench_name: str,
    cap_config: Path,
    cap_label: str,
    workdir: Path,
    saleae_manager: Manager,
    halt_mode: str,
    cpu_freq: int,
    capture_timeout_seconds: float,
    baseline_result: str,
) -> BenchResult:
    """Verify one algorithm for a single (benchmark, capacitor) pair."""
    algorithm = spec.name

    # -- B: Compile instrumented --
    try:
        inst = spec.compile_instrumented(
            tc,
            env,
            bench_path,
            workdir,
            cap_config,
            halt_mode,
            cpu_freq,
            list(VERIFY_DEFINES),
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
                algorithm_result=None,
            )
        msg = f"{algorithm} compilation failed"
        logger.debug("  %s: %s", msg, compile_output[:200])
        return BenchResult(
            bench_name,
            cap_label,
            Status.ERROR,
            msg,
            baseline_result=baseline_result,
            algorithm_result=None,
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
            algorithm_result=None,
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
            algorithm_result=None,
        )

    # -- D: Flash + read instrumented --
    try:
        saleae_run(
            inst_elf,
            saleae_manager,
            FLASH_TIMEOUT,
            AFTER_TRIGGER_SECONDS,
            capture_timeout_seconds,
        )
        time.sleep(POST_CAPTURE_SETTLE_SECONDS)
        inst_nvm = read_nvm(tc, inst_elf, FLASH_TIMEOUT, spec.nvm_symbols)
        raise_if_region_violation(inst_nvm, inst_elf)
    except RegionViolationError as exc:
        msg = f"{algorithm} region energy violation: {exc}"
        logger.error("  %s", msg)
        return BenchResult(
            bench_name,
            cap_label,
            Status.ERROR,
            msg,
            baseline_result=baseline_result,
            algorithm_result=None,
        )
    except (DeviceError, OSError) as exc:
        # A mid-region reset parks the device blinking, so the capture
        # times out — check the violation flag before blaming the device.
        try:
            check_region_violation(tc, inst_elf, FLASH_TIMEOUT)
        except RegionViolationError as vexc:
            msg = f"{algorithm} region energy violation: {vexc}"
            logger.error("  %s", msg)
            return BenchResult(
                bench_name,
                cap_label,
                Status.ERROR,
                msg,
                baseline_result=baseline_result,
                algorithm_result=None,
            )
        except DeviceError, OSError:
            pass
        msg = f"{algorithm} flash/read failed: {exc}"
        logger.error("  %s", msg)
        return BenchResult(
            bench_name,
            cap_label,
            Status.ERROR,
            msg,
            baseline_result=baseline_result,
            algorithm_result=None,
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
            algorithm_result=None,
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
            algorithm_result=None,
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
