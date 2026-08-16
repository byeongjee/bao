"""Benchmark runner for programs powered by a replayed harvesting trace.

``bench`` measures a program under continuous power. This path instead drives
the target from an Otii Ace replaying a recorded harvester trace, so the
program actually loses power and recovers. Every timing column therefore means
wall-clock time including outages, which is why the CSV is kept separate from
the bench one rather than reusing it.

Per (benchmark, capacitor) the program is compiled once with
``halt_mode="wait"`` (region boundaries wait for VCC to recover instead of
emulating outages); per trace it is then flashed through the switchboard,
isolated from the debugger, and run on the replayed supply. The Saleae
capture on P3.4 detects the BENCH_EXIT stop pulse (completion + execution
time) and ends the replay early once it fires; NVM counters are read back
over the reconnected debugger afterwards.
"""

from __future__ import annotations

import csv
import json
import logging
from contextlib import closing
from pathlib import Path

from ..bench.config import (
    CapacitorConfig,
    default_energy_config,
    discover_benchmarks,
    discover_capacitors,
)
from ..bench.runner import (
    AFTER_TRIGGER_SECONDS,
    FLASH_TIMEOUT,
    BenchmarkRow,
    CompileResult,
    write_csv_row,
)
from ..bench.schematic import collect_trace
from ..device.flash import flash_and_hold, read_nvm
from ..device.otii import (
    OtiiSession,
    connect_debugger,
    isolate_target,
    otii_session,
    replay_trace,
)
from ..device.saleae import (
    capture_completion_watcher,
    discover_saleae,
    finish_pulse_capture,
    start_pulse_capture,
)
from ..env import ProjectEnv
from ..errors import CompilationError, ConfigError, DeviceError
from ..output_parser import (
    PassStatistics,
    detect_infeasibility,
    load_stats_json,
    parse_pass_output,
)
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain

logger = logging.getLogger(__name__)

_HALT_MODE = "wait"

# After the replay the target is unpowered, so the armed Saleae capture
# either already triggered (and only its after-trigger tail remains) or
# never will.
_CAPTURE_FINISH_TIMEOUT_SECONDS = 5.0

CSV_HEADER: list[str] = [
    "benchmark",
    "capacitor",
    "trace",
    "status",
    "region_boundaries",
    "compilation_time_ms",
    "profiling_time_ms",
    "replay_seconds",
    "execution_time_us",
    "runtime_region_boundary_calls",
    "runtime_recovery_boots",
    "result",
]

# __nvm_violation, __nvm_done, and cnt_recovery are defined unconditionally
# by every runtime; the result and the remaining counters exist only in
# DEVICE_DEBUG builds.
_BASE_NVM_SYMBOLS = ["__nvm_violation", "__nvm_done", "cnt_recovery"]
_DEBUG_NVM_SYMBOLS = ["__nvm_result", "cnt_boundary"]


def resolve_traces(env: ProjectEnv, trace_specs: list[str]) -> list[Path]:
    """Resolve trace specs to CSV paths.

    A spec is either a path to a trace CSV or a name (e.g. ``1``) resolved
    against ``benchmarks/traces/``.
    """
    trace_dir = env.project_dir / "benchmarks" / "traces"
    paths: list[Path] = []
    for spec in trace_specs:
        candidate = Path(spec)
        if not candidate.is_file():
            name = spec if spec.endswith(".csv") else f"{spec}.csv"
            candidate = trace_dir / name
        if not candidate.is_file():
            raise ConfigError(f"Power trace not found: {spec} (looked in {trace_dir})")
        paths.append(candidate)
    return paths


def load_trace(path: Path) -> list[tuple[float, float]]:
    """Load a ``time_s,voltage_v`` trace CSV."""
    samples: list[tuple[float, float]] = []
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None or [c.strip() for c in header] != ["time_s", "voltage_v"]:
            raise ConfigError(
                f"Unexpected trace CSV header in {path}: {header!r} "
                "(expected time_s,voltage_v)"
            )
        for row in reader:
            if not row:
                continue
            samples.append((float(row[0]), float(row[1])))
    if not samples:
        raise ConfigError(f"Power trace is empty: {path}")
    return samples


def _make_compile_fn(
    algorithm: str,
    env: ProjectEnv,
    tc: Toolchain,
    workdir: Path,
    *,
    energy_config: Path,
    estimator_mode: str,
    cpu_freq: int,
    device_debug: bool,
    max_unroll: int | None,
    pass_log_level: str,
):
    """Build the per-(benchmark, capacitor) compile function for *algorithm*.

    Mirrors the option sets used by ``ckpt bench`` but always links with
    ``halt_mode="wait"``.
    """
    if algorithm == "milp":
        from ..compile.milp import MilpCompileOptions, compile_milp

        def compile_fn(bench_path: Path, cap: CapacitorConfig) -> CompileResult:
            bench_name = bench_path.stem
            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)
            opts = MilpCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                milp_config=cap.config_path,
                output=out_dir / bench_name,
                estimator_mode=estimator_mode,
                pass_log_level=pass_log_level,
                debug=False,
                link=True,
                halt_mode=_HALT_MODE,
                device_debug=device_debug,
                cpu_freq=cpu_freq,
                opt_level=3,
                clang_opt_level=3,
                milp_gap=0.0,
                milp_log_file="",
                coarse_allocation=False,
                save_temps=False,
            )
            result = compile_milp(tc, env, opts)
            return CompileResult(
                out_dir=out_dir,
                pass_output=result.pass_output,
                stats_json=result.stats_json,
                profiling_time_ms=result.profiling_time_ms,
            )

        return compile_fn

    if algorithm == "rockclimb":
        from ..compile.rockclimb import RockClimbCompileOptions, compile_rockclimb

        def compile_fn(bench_path: Path, cap: CapacitorConfig) -> CompileResult:
            bench_name = bench_path.stem
            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)
            opts = RockClimbCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                rockclimb_config=cap.config_path,
                output=out_dir / bench_name,
                pass_log_level=pass_log_level,
                precomputed_energy=True,
                link=True,
                device_debug=device_debug,
                halt_mode=_HALT_MODE,
                cpu_freq=cpu_freq,
                clang_opt_level=3,
                opt_level=3,
                max_unroll=max_unroll,
                save_temps=False,
                linker_script=None,
            )
            result = compile_rockclimb(tc, env, opts)
            return CompileResult(
                out_dir=out_dir,
                pass_output=result.pass_output,
                stats_json=result.stats_json,
                profiling_time_ms=0,
            )

        return compile_fn

    if algorithm in ("schematic", "schematicO3"):
        from ..compile.schematic import (
            CLANG_OPT_LEVEL_BY_LABEL,
            SchematicCompileOptions,
            compile_schematic,
        )

        clang_opt_level = CLANG_OPT_LEVEL_BY_LABEL[algorithm]
        trace_config = env.project_dir / "benchmarks" / "config_10uF.json"
        trace_cache: dict[str, tuple[Path, int]] = {}

        def compile_fn(bench_path: Path, cap: CapacitorConfig) -> CompileResult:
            bench_name = bench_path.stem
            if bench_name not in trace_cache:
                trace_cache[bench_name] = collect_trace(
                    tc,
                    env,
                    bench_path,
                    workdir,
                    energy_config=energy_config,
                    trace_config=trace_config,
                    estimator_mode=estimator_mode,
                    halt_mode=_HALT_MODE,
                    device_debug=device_debug,
                    cpu_freq=cpu_freq,
                    clang_opt_level=clang_opt_level,
                    pass_log_level=pass_log_level,
                )
            trace_json, profiling_ms = trace_cache[bench_name]

            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)
            opts = SchematicCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                schematic_config=cap.config_path,
                output=out_dir / bench_name,
                estimator_mode=estimator_mode,
                pass_log_level=pass_log_level,
                debug=False,
                trace_only=False,
                link=True,
                device_debug=device_debug,
                halt_mode=_HALT_MODE,
                cpu_freq=cpu_freq,
                opt_level=3,
                clang_opt_level=clang_opt_level,
                save_temps=False,
                trace_file=trace_json,
                linker_script=None,
                extra_includes=[str(env.project_dir / "passes" / "runtime")],
            )
            result = compile_schematic(tc, env, opts)
            return CompileResult(
                out_dir=out_dir,
                pass_output=result.pass_output,
                stats_json=result.stats_json,
                profiling_time_ms=profiling_ms,
            )

        return compile_fn

    raise ConfigError(f"Unknown algorithm: {algorithm!r}")


def _parse_compile_stats(
    compile_result: CompileResult,
) -> tuple[PassStatistics | None, str | None]:
    """Return (stats, infeasible_reason) from a compile result."""
    if compile_result.stats_json is not None and compile_result.stats_json.is_file():
        with open(compile_result.stats_json) as f:
            data = json.load(f)
        stats, feasible, reason = load_stats_json(data)
        return stats, None if feasible else (reason or "infeasible")
    reason = detect_infeasibility(compile_result.pass_output)
    return parse_pass_output(compile_result.pass_output), reason


def _run_on_trace(
    tc: Toolchain,
    otii: OtiiSession,
    saleae_manager,
    elf: Path,
    samples: list[tuple[float, float]],
    nvm_symbols: list[str],
) -> tuple[float, bool, float | None, dict[str, int]]:
    """Flash, isolate, replay one trace, then reconnect and read NVM.

    Returns ``(replay_seconds, completed, execution_time_us, nvm_values)``.
    """
    connect_debugger(otii)
    session = flash_and_hold(elf, FLASH_TIMEOUT)

    # Arm the stop-pulse capture while the target is still halted, so the
    # whole replayed run happens inside the capture window.
    try:
        capture_context = start_pulse_capture(saleae_manager, AFTER_TRIGGER_SECONDS)
    except Exception:
        session.abort()
        raise

    with capture_context as capture:
        # Open the relays while the target is still halted under JTAG: it
        # loses power without ever running on the debugger's 3V3 rail, so
        # the NVM state stays exactly as programmed. The mspdebug session
        # dies with the cut SBW lines; abort() discards it — and must run
        # even when isolate_target fails, or the leaked session keeps the
        # ez-FET claimed for every following trace.
        try:
            isolate_target(otii)
        finally:
            session.abort()

        with capture_completion_watcher(capture) as benchmark_done:
            replay_seconds = replay_trace(otii, samples, benchmark_done)
        completed, execution_time_us = finish_pulse_capture(
            capture, _CAPTURE_FINISH_TIMEOUT_SECONDS
        )

    connect_debugger(otii)
    values = read_nvm(tc, elf, FLASH_TIMEOUT, nvm_symbols)
    return replay_seconds, completed, execution_time_us, values


def _build_row(
    bench_name: str,
    cap_label: str,
    trace_label: str,
    status: str,
    fields: dict[str, str | int | None],
) -> BenchmarkRow:
    fields["trace"] = trace_label
    return BenchmarkRow(
        benchmark=bench_name,
        capacitor=cap_label,
        status=status,
        fields=fields,
    )


def run_intermittent_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    algorithm: str,
    benchmarks: list[str] | None,
    caps: list[str],
    trace_specs: list[str],
    output_csv: Path | None,
    device_debug: bool,
    estimator_mode: str,
    cpu_freq: int,
    max_unroll: int | None,
    pass_log_level: str,
) -> None:
    """Run *algorithm* over (benchmark x capacitor x trace) under replayed power."""
    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        raise ConfigError("No benchmarks to run")
    capacitors = discover_capacitors(env, algorithm, caps)
    traces = [(p.stem, load_trace(p)) for p in resolve_traces(env, trace_specs)]

    if output_csv is None:
        output_csv = (
            env.project_dir / "benchmarks" / f"intermittent_{algorithm}_summary.csv"
        )

    energy_config = default_energy_config(env, algorithm)
    if algorithm == "milp" and estimator_mode == "ir":
        energy_config = env.project_dir / "benchmarks" / "sample_energy_config_ir.json"

    nvm_symbols = list(_BASE_NVM_SYMBOLS)
    if device_debug:
        nvm_symbols += _DEBUG_NVM_SYMBOLS

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with (
        closing(discover_saleae()) as saleae_manager,
        otii_session() as otii,
        compilation_workdir(prefix=f"intermittent_{algorithm}_") as workdir,
        open(output_csv, "w", newline="") as csvfile,
    ):
        writer = csv.writer(csvfile)
        writer.writerow(CSV_HEADER)

        def emit(row: BenchmarkRow) -> None:
            # Flush per row: each one costs a compile and/or a full trace
            # replay, so an interrupted run must not lose finished rows.
            write_csv_row(writer, row, CSV_HEADER)
            csvfile.flush()

        compile_fn = _make_compile_fn(
            algorithm,
            env,
            tc,
            workdir,
            energy_config=energy_config,
            estimator_mode=estimator_mode,
            cpu_freq=cpu_freq,
            device_debug=device_debug,
            max_unroll=max_unroll,
            pass_log_level=pass_log_level,
        )

        total = len(bench_paths) * len(capacitors) * len(traces)
        count = 0
        for bench_path in bench_paths:
            bench_name = bench_path.stem
            for cap in capacitors:
                logger.info("Compiling %s-%s ...", bench_name, cap.label)
                try:
                    compile_result = compile_fn(bench_path, cap)
                except CompilationError as exc:
                    logger.error("  FAILED (compilation): %s", exc)
                    for trace_label, _ in traces:
                        count += 1
                        emit(
                            _build_row(bench_name, cap.label, trace_label, "failed", {})
                        )
                    continue

                stats, infeasible_reason = _parse_compile_stats(compile_result)
                if infeasible_reason is not None:
                    logger.error("  INFEASIBLE (%s)", infeasible_reason)
                    for trace_label, _ in traces:
                        count += 1
                        emit(
                            _build_row(
                                bench_name, cap.label, trace_label, "infeasible", {}
                            )
                        )
                    continue

                static_fields: dict[str, str | int | None] = {
                    "region_boundaries": (stats.region_boundaries or 0) if stats else 0,
                    "compilation_time_ms": (stats.compilation_time_ms or 0)
                    if stats
                    else 0,
                    "profiling_time_ms": compile_result.profiling_time_ms,
                }

                elf = compile_result.out_dir / f"{bench_name}.elf"
                if not elf.is_file():
                    logger.error("  FAILED (no ELF produced)")
                    for trace_label, _ in traces:
                        count += 1
                        emit(
                            _build_row(
                                bench_name,
                                cap.label,
                                trace_label,
                                "failed",
                                dict(static_fields),
                            )
                        )
                    continue

                for trace_label, samples in traces:
                    count += 1
                    logger.info(
                        "[%d/%d] Running %s-%s on trace %s ...",
                        count,
                        total,
                        bench_name,
                        cap.label,
                        trace_label,
                    )
                    fields = dict(static_fields)
                    try:
                        replay_seconds, completed, execution_time_us, values = (
                            _run_on_trace(
                                tc, otii, saleae_manager, elf, samples, nvm_symbols
                            )
                        )
                    except DeviceError as exc:
                        logger.error("  DEVICE ERROR: %s", exc)
                        emit(
                            _build_row(
                                bench_name,
                                cap.label,
                                trace_label,
                                "device_error",
                                fields,
                            )
                        )
                        continue

                    # The stop pulse alone can be spoofed by a marginal-VCC
                    # boot stretching the start pulse past 1 ms, so a
                    # completion also requires the committed done flag.
                    done = values.get("__nvm_done", 0) == 1
                    if values.get("__nvm_violation", 0) != 0:
                        status = "region_violation"
                    elif completed and done:
                        status = "ok"
                    elif completed:
                        status = "unconfirmed"
                    else:
                        status = "incomplete"

                    fields["replay_seconds"] = str(round(replay_seconds, 2))
                    if status == "ok" and execution_time_us is not None:
                        fields["execution_time_us"] = str(round(execution_time_us, 2))
                    # Counters are trustworthy only when the device parked at
                    # readback: done committed (ok) or violation park. Any
                    # other run resumes under debugger power the moment the
                    # relays close, so its counters and result no longer
                    # describe the replayed run — blank them.
                    if status in ("ok", "region_violation"):
                        fields["runtime_recovery_boots"] = values.get("cnt_recovery")
                        if device_debug:
                            fields["runtime_region_boundary_calls"] = values.get(
                                "cnt_boundary"
                            )
                            if status == "ok":
                                fields["result"] = values.get("__nvm_result")

                    emit(_build_row(bench_name, cap.label, trace_label, status, fields))
                    if status == "ok":
                        logger.info("  OK")
                    else:
                        logger.error("  %s", status.upper())

    logger.info("")
    logger.info("==========================================")
    logger.info("Results written to: %s", output_csv)
    logger.info("==========================================")
