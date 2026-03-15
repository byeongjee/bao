"""RockClimb benchmark runner -- replaces run_rockclimb.sh.

Iterates over (benchmark x capacitor), compiles with the machine-level
RockClimb (PFI) pass via ``llc``, optionally flashes to an MSP430 device,
and writes a CSV summary of pass statistics and runtime counters.
"""

from __future__ import annotations

from pathlib import Path

from ..compile.rockclimb import (
    RockClimbCompileOptions,
    RockClimbCompileResult,
    compile_rockclimb,
)
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
from .runner import build_base_fields, check_device_available, nvm_counter, run_benchmark_matrix

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
    "boundary_checks",
    "runtime_region_boundary_calls",
    "runtime_debug_save_reg_calls",
    "runtime_debug_restore_reg_calls",
    "result",
]

_NVM_SYMBOLS: list[str] = [
    "__nvm_done",
    "__nvm_result",
    "cnt_boundary",
    "cnt_save_reg",
    "cnt_restore_reg",
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
    fields.update({
        "profiling_time_ms": "",
        "boundary_checks": stats.boundary_checks or 0,
        "runtime_region_boundary_calls": nvm_counter(nvm, "region_boundary"),
        "runtime_debug_save_reg_calls": nvm_counter(nvm, "save_reg"),
        "runtime_debug_restore_reg_calls": nvm_counter(nvm, "restore_reg"),
    })
    return fields


def run_rockclimb_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None = None,
    caps: list[str] | None = None,
    output_csv: Path | None = None,
    device_debug: bool,
    halt_mode: str,
    energy_config: Path | None = None,
    cpu_freq: int,
) -> None:
    """Run RockClimb checkpoint insertion across all benchmarks and capacitor sizes.

    This is the Python equivalent of ``scripts/run_rockclimb.sh``.

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
        ``benchmarks/rockclimb_benchmark_summary.csv``.
    device_debug:
        Link the debug-counter runtime and attempt NVM readback.
    """
    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        raise ConfigError("No benchmarks to run")

    capacitors = discover_capacitors(env, "rockclimb", caps)

    if output_csv is None:
        output_csv = env.project_dir / "benchmarks" / "rockclimb_benchmark_summary.csv"

    if energy_config is None:
        energy_config = default_energy_config(env, "rockclimb")

    if not check_device_available():
        raise ConfigError("No MSP430 device detected")

    from ..device.saleae import discover_saleae

    saleae_manager = discover_saleae()

    with compilation_workdir(prefix="rockclimb_bench_") as workdir:

        def compile_fn(
            bench_path: Path, cap: CapacitorConfig
        ) -> tuple[Path, str, Path | None]:
            bench_name = bench_path.stem
            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)

            opts = RockClimbCompileOptions(
                input_c=bench_path,
                energy_config=energy_config,
                rockclimb_config=cap.config_path,
                output=out_dir / bench_name,
                pass_log_level="info",
                precomputed_energy=True,
                link=True,
                device_debug=device_debug,
                halt_mode=halt_mode,
                cpu_freq=cpu_freq,
            )

            result: RockClimbCompileResult = compile_rockclimb(tc, env, opts)
            return out_dir, result.pass_output, result.stats_json

        run_benchmark_matrix(
            env,
            tc,
            bench_paths,
            capacitors,
            compile_fn,
            output_csv,
            nvm_symbols=_NVM_SYMBOLS,
            device_debug=device_debug,
            csv_header=_CSV_HEADER,
            row_builder=_build_row,
            saleae_manager=saleae_manager,
        )
