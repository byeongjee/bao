"""SCHEMATIC benchmark runner -- replaces run_schematic.sh.

SCHEMATIC uses a two-phase approach per benchmark:

1. **Trace collection** -- run once per benchmark with ``--trace-only``
   and ``-Oc 0`` using a dummy (10uF) config.
2. **Per-capacitor compilation** -- compile with ``--trace <collected_trace>``
   ``--link`` ``--add-debug-markers`` for each capacitor size.

Optionally flashes to an MSP430 device and writes a CSV summary.
"""

from __future__ import annotations

from pathlib import Path

from ..compile.schematic import (
    SchematicCompileOptions,
    SchematicCompileResult,
    compile_schematic,
)
from ..env import ProjectEnv
from ..output_parser import (
    NvmCounters,
    PassStatistics,
    extract_stat,
)
from ..errors import ConfigError
from ..runner import CompilationError, StepResult
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import (
    CapacitorConfig,
    default_energy_config,
    discover_benchmarks,
    discover_capacitors,
)
from .runner import (
    BenchmarkSkipped,
    build_base_fields,
    check_device_available,
    nvm_counter,
    run_benchmark_matrix,
)

_CSV_HEADER: list[str] = [
    "benchmark",
    "capacitor",
    "status",
    "basic_blocks",
    "edges",
    "regions",
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
    "region_boundaries",
    "enabled_checkpoints",
    "loop_decisions",
    "paths_analyzed",
    "runtime_calls_inserted",
]

_NVM_SYMBOLS: list[str] = [
    "__nvm_done",
    "__nvm_result",
    "cnt_boundary",
    "cnt_save_reg",
    "cnt_restore_reg",
    "cnt_store_mem",
    "cnt_restore_mem",
]


def _build_row(
    bench_name: str,
    cap_label: str,
    stats: PassStatistics,
    nvm: NvmCounters | None,
    full_output: str,
    *,
    profiling_time_ms: int = 0,
) -> dict[str, str | int | None]:
    """Build a CSV row dict from parsed statistics and NVM counters."""
    fields = build_base_fields(stats, full_output, nvm)
    fields.update({
        "profiling_time_ms": profiling_time_ms,
        "runtime_region_boundary_calls": nvm_counter(nvm, "region_boundary"),
        "runtime_debug_save_reg_calls": nvm_counter(nvm, "save_reg"),
        "runtime_debug_restore_reg_calls": nvm_counter(nvm, "restore_reg"),
        "runtime_debug_store_mem_calls": nvm_counter(nvm, "store_mem"),
        "runtime_debug_restore_mem_calls": nvm_counter(nvm, "restore_mem"),
        "candidate_globals": stats.candidate_globals or 0,
        "region_boundaries": stats.region_boundaries or 0,
        "enabled_checkpoints": stats.enabled_checkpoints or 0,
        "loop_decisions": stats.loop_decisions or 0,
        "paths_analyzed": stats.paths_analyzed or 0,
        "runtime_calls_inserted": stats.runtime_calls_inserted or 0,
    })
    return fields


def run_schematic_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None = None,
    caps: list[str] | None = None,
    output_csv: Path | None = None,
    debug_counters: bool,
    halt_mode: str,
    verbose: bool,
    energy_config: Path | None = None,
    trace_config: Path | None = None,
    estimator_mode: str,
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
    debug_counters:
        Link the debug-counter runtime and attempt NVM readback.
    halt_mode:
        Halt mode at region boundaries (nop/bor/lpm4).
    verbose:
        Print full compiler output for each benchmark.
    """
    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        raise ConfigError("No benchmarks to run")

    capacitors = discover_capacitors(env, "schematic", caps)

    if output_csv is None:
        output_csv = env.project_dir / "benchmarks" / "schematic_benchmark_summary.csv"

    if energy_config is None:
        energy_config = default_energy_config(env, "schematic")

    # Config for trace collection phase (any capacitor works; 10uF by default)
    if trace_config is None:
        trace_config = env.project_dir / "benchmarks" / "config_10uF.json"

    from ..device.saleae import discover_saleae

    saleae_manager = discover_saleae()
    if not check_device_available():
        raise ConfigError("No MSP430 device detected")

    # State shared between pre_benchmark and compile_fn/row_builder
    profiling_time_ms = 0
    trace_json: Path | None = None
    with compilation_workdir(prefix="schematic_bench_") as workdir:

        def pre_benchmark(bench_path: Path) -> None:
            nonlocal profiling_time_ms, trace_json
            bench_name = bench_path.stem

            print()
            print(f"--- Collecting trace for {bench_name} ---")

            profiling_time_ms = 0
            trace_json = workdir / f"{bench_name}_trace.json"

            try:
                trace_opts = SchematicCompileOptions(
                    input_c=bench_path,
                    energy_config=energy_config,
                    schematic_config=trace_config,
                    output=workdir / bench_name,
                    estimator_mode=estimator_mode,
                    verbose=verbose,
                    debug=False,
                    add_debug_markers=False,
                    trace_only=True,
                    link=False,
                    debug_counters=False,
                    halt_mode=halt_mode,
                    clang_opt_level=0,
                    extra_includes=[str(env.project_dir / "passes" / "runtime")],
                )
                trace_result: SchematicCompileResult = compile_schematic(
                    tc, env, trace_opts
                )

                if verbose:
                    print(trace_result.pass_output)

                # Extract profiling time from trace output
                raw_prof = extract_stat(trace_result.pass_output, "Profiling time (ms)")
                if raw_prof is not None:
                    try:
                        profiling_time_ms = int(raw_prof)
                    except ValueError:
                        pass

                if not trace_json.is_file():
                    raise CompilationError(
                        "trace-collect",
                        StepResult(
                            returncode=0,
                            stdout="",
                            stderr=f"Trace file not produced: {trace_json}",
                            duration_ms=0,
                        ),
                    )

                print(f"  Trace collected for {bench_name} (profiling: {profiling_time_ms}ms)")

            except (CompilationError, OSError) as exc:
                print(f"  FAILED: trace collection for {bench_name}")
                if verbose:
                    if isinstance(exc, CompilationError) and exc.result:
                        print(exc.result.output[-500:])
                    else:
                        print(str(exc))
                raise BenchmarkSkipped("TRACE_FAILED") from exc

        def compile_fn(
            bench_path: Path, cap: CapacitorConfig
        ) -> tuple[Path, str, Path | None]:
            bench_name = bench_path.stem
            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)

            compile_opts = SchematicCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                schematic_config=cap.config_path,
                output=out_dir / bench_name,
                estimator_mode=estimator_mode,
                verbose=verbose,
                debug=False,
                add_debug_markers=True,
                trace_only=False,
                link=True,
                debug_counters=debug_counters,
                halt_mode=halt_mode,
                clang_opt_level=0,
                extra_includes=[str(env.project_dir / "passes" / "runtime")],
                trace_file=trace_json,
            )
            result: SchematicCompileResult = compile_schematic(
                tc, env, compile_opts
            )
            return out_dir, result.pass_output, result.stats_json

        def row_builder(
            bench_name: str,
            cap_label: str,
            stats: PassStatistics,
            nvm: NvmCounters | None,
            full_output: str,
        ) -> dict[str, str | int | None]:
            return _build_row(
                bench_name, cap_label, stats, nvm, full_output,
                profiling_time_ms=profiling_time_ms,
            )

        run_benchmark_matrix(
            env,
            tc,
            bench_paths,
            capacitors,
            compile_fn,
            output_csv,
            nvm_symbols=_NVM_SYMBOLS,
            debug_counters=debug_counters,
            verbose=verbose,
            csv_header=_CSV_HEADER,
            row_builder=row_builder,
            pre_benchmark=pre_benchmark,
            saleae_manager=saleae_manager,
        )
