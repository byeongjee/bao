"""MILP benchmark runner -- replaces run_milp.sh.

Iterates over (benchmark x capacitor), compiles with the MILP checkpoint
insertion pass, optionally flashes to an MSP430 device, and writes a CSV
summary of pass statistics and runtime counters.
"""

from __future__ import annotations

from pathlib import Path

from ..compile.milp import MilpCompileOptions, MilpCompileResult, compile_milp
from ..env import ProjectEnv
from ..errors import ConfigError
from ..output_parser import (
    NvmCounters,
    PassStatistics,
)

from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import (
    CapacitorConfig,
    default_energy_config,
    discover_benchmarks,
    discover_capacitors,
)
from .runner import build_base_fields, nvm_counter, run_benchmark_matrix

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
    "runtime_region_boundary_calls",
    "runtime_debug_save_vreg_calls",
    "runtime_debug_restore_vreg_calls",
    "runtime_debug_store_mem_calls",
    "runtime_debug_restore_mem_calls",
    "candidate_globals",
    "milp_variables",
    "milp_constraints",
    "optimal_solution",
    "region_boundaries_inserted",
    "distributed_checkpoints_inserted",
    "milp_solve_time_ms",
    "result",
]

_NVM_SYMBOLS: list[str] = [
    "__nvm_done",
    "__nvm_result",
    "cnt_boundary",
    "cnt_save_vreg",
    "cnt_restore_vreg",
    "cnt_store_mem",
    "cnt_restore_mem",
]


def _build_row(
    bench_name: str,
    cap_label: str,
    stats: PassStatistics,
    nvm: NvmCounters | None,
    full_output: str,
) -> dict[str, str | int | None]:
    """Build a CSV row dict from parsed statistics and NVM counters."""
    fields = build_base_fields(stats, full_output, nvm)
    optimal = stats.optimal_solution
    if optimal is not None:
        optimal = "yes" if optimal == "yes" else "no"
    fields.update({
        "profiling_time_ms": stats.profiling_time_ms or 0,
        "runtime_region_boundary_calls": nvm_counter(nvm, "region_boundary"),
        "runtime_debug_save_vreg_calls": nvm_counter(nvm, "save_vreg"),
        "runtime_debug_restore_vreg_calls": nvm_counter(nvm, "restore_vreg"),
        "runtime_debug_store_mem_calls": nvm_counter(nvm, "store_mem"),
        "runtime_debug_restore_mem_calls": nvm_counter(nvm, "restore_mem"),
        "candidate_globals": stats.candidate_globals or 0,
        "milp_variables": stats.milp_variables or 0,
        "milp_constraints": stats.milp_constraints or 0,
        "optimal_solution": optimal,
        "region_boundaries_inserted": stats.region_boundaries or 0,
        "distributed_checkpoints_inserted": stats.distributed_checkpoints or 0,
        "milp_solve_time_ms": stats.solve_time_ms or 0,
    })
    return fields


def run_milp_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None = None,
    caps: list[str] | None = None,
    output_csv: Path | None = None,
    debug_counters: bool,
    halt_mode: str,
    verbose: bool,
    estimator_mode: str,
    energy_config: Path | None = None,
) -> None:
    """Run MILP checkpoint insertion across all benchmarks and capacitor sizes.

    This is the Python equivalent of ``scripts/run_milp.sh``.

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
        ``benchmarks/milp_benchmark_summary.csv``.
    debug_counters:
        Link the debug-counter runtime and attempt NVM readback.
    verbose:
        Print full compiler output for each benchmark.
    estimator_mode:
        ``"assembly"`` (default) or ``"ir"``.
    energy_config:
        Override the energy estimator config path.  Defaults are chosen
        based on *estimator_mode*.
    """
    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        raise ConfigError("No benchmarks to run")

    capacitors = discover_capacitors(env, "milp", caps)

    if output_csv is None:
        output_csv = env.project_dir / "benchmarks" / "milp_benchmark_summary.csv"

    if energy_config is None:
        if estimator_mode == "ir":
            energy_config = env.project_dir / "benchmarks" / "sample_energy_config_ir.json"
        else:
            energy_config = default_energy_config(env, "milp")

    # Shared workdir for all compilations (cleaned up on exit)
    with compilation_workdir(prefix="milp_bench_") as workdir:

        def compile_fn(
            bench_path: Path, cap: CapacitorConfig
        ) -> tuple[Path, str, Path | None]:
            bench_name = bench_path.stem
            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)

            opts = MilpCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                milp_config=cap.config_path,
                output=out_dir / bench_name,
                estimator_mode=estimator_mode,
                verbose=True,
                debug=False,
                add_debug_markers=True,
                link=True,
                halt_mode=halt_mode,
                debug_counters=debug_counters,
            )

            result: MilpCompileResult = compile_milp(tc, env, opts)
            return out_dir, result.pass_output, result.stats_json

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
            row_builder=_build_row,
        )
