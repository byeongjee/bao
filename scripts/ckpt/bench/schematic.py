"""SCHEMATIC benchmark runner -- replaces run_schematic.sh.

SCHEMATIC uses a two-phase approach per benchmark:

1. **Trace collection** -- run once per benchmark with ``--trace-only``
   and ``-Oc 0`` using a dummy (10uF) config.
2. **Per-capacitor compilation** -- compile with ``--trace <collected_trace>``
   ``--link`` for each capacitor size.

Optionally flashes to an MSP430 device and writes a CSV summary.
"""

from __future__ import annotations

import logging
from pathlib import Path

from ..compile.schematic import (
    SchematicCompileOptions,
    SchematicCompileResult,
    compile_schematic,
)
from ..env import ProjectEnv
from ..errors import CompilationError, ConfigError
from ..output_parser import (
    NvmCounters,
    PassStatistics,
)
from ..runner import StepResult
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import (
    CapacitorConfig,
    default_energy_config,
    discover_benchmarks,
    discover_capacitors,
)
from .runner import (
    MATRIX_COMPILE_ONLY_WARNING,
    CompileResult,
    nvm_counter,
    optional_saleae,
    run_benchmark_matrix,
)

logger = logging.getLogger(__name__)

CSV_HEADER: list[str] = [
    "benchmark",
    "capacitor",
    "status",
    "basic_blocks",
    "edges",
    "region_boundaries",
    "compilation_time_ms",
    "peak_rss_kb",
    "profiling_time_ms",
    "execution_time_us",
    "runtime_region_boundary_calls",
    "runtime_debug_save_reg_calls",
    "runtime_debug_restore_reg_calls",
    "runtime_debug_store_mem_calls",
    "runtime_debug_restore_mem_calls",
    "result",
    "candidate_globals",
    "enabled_checkpoints",
    "loop_decisions",
    "paths_analyzed",
    "runtime_calls_inserted",
    "run_attempts",
]

NVM_SYMBOLS: list[str] = [
    "__nvm_done",
    "__nvm_result",
    "__nvm_violation",
    "cnt_boundary",
    "cnt_save_reg",
    "cnt_restore_reg",
    "cnt_store_mem",
    "cnt_restore_mem",
]


def collect_trace(
    tc: Toolchain,
    env: ProjectEnv,
    bench_path: Path,
    workdir: Path,
    *,
    energy_config: Path,
    trace_config: Path,
    estimator_mode: str,
    halt_mode: str,
    device_debug: bool,
    cpu_freq: int,
    clang_opt_level: int,
    pass_log_level: str,
    extra_defines: list[str],
) -> tuple[Path, int]:
    """Collect a SCHEMATIC execution trace for one benchmark.

    *device_debug* and *extra_defines* must match the build the trace is
    applied to: they rename blocks in ``main``, and TraceLoader drops traces
    naming unknown blocks.

    Returns (trace_json_path, profiling_time_ms).
    Raises CompilationError if trace collection fails.
    """
    bench_name = bench_path.stem

    logger.info("")
    logger.info("--- Collecting trace for %s ---", bench_name)

    trace_opts = SchematicCompileOptions(
        input_c=bench_path,
        energy_config=energy_config,
        schematic_config=trace_config,
        output=workdir / bench_name,
        estimator_mode=estimator_mode,
        pass_log_level=pass_log_level,
        debug=False,
        trace_only=True,
        link=False,
        device_debug=device_debug,
        halt_mode=halt_mode,
        cpu_freq=cpu_freq,
        opt_level=3,
        clang_opt_level=clang_opt_level,
        save_temps=False,
        trace_file=None,
        linker_script=None,
        extra_includes=[str(env.project_dir / "passes" / "runtime")],
        extra_defines=list(extra_defines),
    )
    trace_result: SchematicCompileResult = compile_schematic(tc, env, trace_opts)

    logger.debug("Trace output:\n%s", trace_result.pass_output)

    profiling_time_ms = trace_result.profiling_time_ms

    trace_json = trace_result.trace_file
    if trace_json is None or not trace_json.is_file():
        raise CompilationError(
            "trace-collect",
            StepResult(
                returncode=0,
                stdout="",
                stderr=f"Trace file not produced for {bench_name}",
                duration_ms=0,
            ),
        )

    logger.info(
        "  Trace collected for %s (profiling: %dms)", bench_name, profiling_time_ms
    )
    return trace_json, profiling_time_ms


def build_row(
    bench_name: str,
    cap_label: str,
    stats: PassStatistics,
    nvm: NvmCounters | None,
    full_output: str,
) -> dict[str, str | int | None]:
    """Build SCHEMATIC-specific CSV fields."""
    return {
        "runtime_region_boundary_calls": nvm_counter(nvm, "region_boundary"),
        "runtime_debug_save_reg_calls": nvm_counter(nvm, "save_reg"),
        "runtime_debug_restore_reg_calls": nvm_counter(nvm, "restore_reg"),
        "runtime_debug_store_mem_calls": nvm_counter(nvm, "store_mem"),
        "runtime_debug_restore_mem_calls": nvm_counter(nvm, "restore_mem"),
        "candidate_globals": stats.candidate_globals or 0,
        "enabled_checkpoints": stats.enabled_checkpoints or 0,
        "loop_decisions": stats.loop_decisions or 0,
        "paths_analyzed": stats.paths_analyzed or 0,
        "runtime_calls_inserted": stats.runtime_calls_inserted or 0,
    }


def run_schematic_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    output_csv: Path | None,
    device_debug: bool,
    capture_timeout_seconds: float,
    halt_mode: str,
    energy_config: Path | None,
    trace_config: Path | None,
    estimator_mode: str,
    cpu_freq: int,
    clang_opt_level: int,
    pass_log_level: str,
    algorithm_label: str,
    accumulate_keys_file: Path | None,
) -> None:
    """Run SCHEMATIC checkpoint insertion across all benchmarks and capacitor sizes.

    This is the Python equivalent of ``scripts/run_schematic.sh``.

    SCHEMATIC collects a trace once per benchmark, then reuses it for
    each capacitor size.

    Parameters
    ----------
    benchmarks:
        Optional list of benchmark names (without ``.c``).  If ``None``,
        all benchmarks under ``benchmarks/intermittent/`` are used.
    caps:
        Optional list of capacitor labels (e.g. ``["1uF", "10uF"]``).
        If ``None``, all three default sizes are used.
    output_csv:
        Where to write the CSV summary.  Defaults to
        ``benchmarks/schematic_benchmark_summary.csv``.
    device_debug:
        Link the debug-counter runtime and attempt NVM readback.
    halt_mode:
        Halt mode at region boundaries (bor/swbor/wait).
    """
    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        raise ConfigError("No benchmarks to run")

    capacitors = discover_capacitors(env, algorithm_label, caps)

    if output_csv is None:
        output_csv = (
            env.project_dir / "benchmarks" / f"{algorithm_label}_benchmark_summary.csv"
        )

    if energy_config is None:
        energy_config = default_energy_config(env, algorithm_label)

    # Config for trace collection phase (any capacitor works; 10uF by default)
    if trace_config is None:
        trace_config = env.project_dir / "benchmarks" / "config_10uF.json"

    # Trace cache: collect once per benchmark, reuse for all capacitors.
    trace_cache: dict[
        str, tuple[Path, int]
    ] = {}  # bench_name -> (trace_path, profiling_ms)

    with (
        optional_saleae(MATRIX_COMPILE_ONLY_WARNING) as saleae_manager,
        compilation_workdir(prefix="schematic_bench_") as workdir,
    ):

        def compile_fn(bench_path: Path, cap: CapacitorConfig) -> CompileResult:
            bench_name = bench_path.stem

            # Collect trace on first capacitor for this benchmark
            if bench_name not in trace_cache:
                trace_cache[bench_name] = collect_trace(
                    tc,
                    env,
                    bench_path,
                    workdir,
                    energy_config=energy_config,
                    trace_config=trace_config,
                    estimator_mode=estimator_mode,
                    halt_mode=halt_mode,
                    device_debug=device_debug,
                    cpu_freq=cpu_freq,
                    clang_opt_level=clang_opt_level,
                    pass_log_level=pass_log_level,
                    extra_defines=[],
                )

            trace_json, profiling_ms = trace_cache[bench_name]

            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)

            compile_opts = SchematicCompileOptions(
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
                halt_mode=halt_mode,
                cpu_freq=cpu_freq,
                opt_level=3,
                clang_opt_level=clang_opt_level,
                save_temps=False,
                trace_file=trace_json,
                linker_script=None,
                extra_includes=[str(env.project_dir / "passes" / "runtime")],
            )
            result: SchematicCompileResult = compile_schematic(tc, env, compile_opts)
            return CompileResult(
                out_dir=out_dir,
                pass_output=result.pass_output,
                stats_json=result.stats_json,
                profiling_time_ms=profiling_ms,
            )

        run_benchmark_matrix(
            env,
            tc,
            bench_paths,
            capacitors,
            compile_fn,
            output_csv,
            nvm_symbols=NVM_SYMBOLS,
            device_debug=device_debug,
            csv_header=CSV_HEADER,
            row_builder=build_row,
            saleae_manager=saleae_manager,
            capture_timeout_seconds=capture_timeout_seconds,
            accumulate_keys_file=accumulate_keys_file,
        )
