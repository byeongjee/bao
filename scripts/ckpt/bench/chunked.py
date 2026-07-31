"""Chunking-only benchmark runner — chunked baseline execution time.

Compiles each benchmark with loop chunking applied but no checkpoint
instrumentation, flashes to an MSP430 device, and measures execution time
via Saleae GPIO capture. Chunk sizes depend on the energy budget, so
benchmarks run per capacitor size like the MILP runs.
"""

from __future__ import annotations

import logging
from pathlib import Path

from ..compile.chunked import ChunkedCompileOptions, compile_chunked
from ..env import ProjectEnv
from ..errors import ConfigError
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import (
    CapacitorConfig,
    default_energy_config,
    discover_benchmarks,
    discover_capacitors,
)
from .runner import (
    TIMING_COMPILE_ONLY_WARNING,
    optional_saleae,
    run_timing_matrix,
)

logger = logging.getLogger(__name__)

CSV_HEADER: list[str] = [
    "benchmark",
    "capacitor",
    "status",
    "compilation_time_ms",
    "execution_time_us",
]


def run_chunked_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    output_csv: Path | None,
    energy_config: Path | None,
    capture_timeout_seconds: float,
    cpu_freq: int,
    pass_log_level: str,
    clang_opt_level: int = 3,
    opt_level: int = 3,
) -> None:
    """Compile and measure chunking-only baselines for all benchmarks.

    For each (benchmark, capacitor):
      1. Compile with loop chunking but no checkpoint insertion
      2. Flash to device
      3. Measure execution time via Saleae GPIO capture
      4. Write results to CSV
    """
    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        raise ConfigError("No benchmarks to run")

    capacitors = discover_capacitors(env, "milp", caps)

    if output_csv is None:
        output_csv = env.project_dir / "benchmarks" / "chunked_benchmark_summary.csv"

    if energy_config is None:
        energy_config = default_energy_config(env, "milp")

    with (
        optional_saleae(TIMING_COMPILE_ONLY_WARNING) as saleae_manager,
        compilation_workdir(prefix="chunked_bench_") as workdir,
    ):

        def compile_fn(bench_path: Path, cap: CapacitorConfig | None) -> Path | None:
            assert cap is not None  # chunked always runs per capacitor
            bench_name = bench_path.stem
            out_dir = workdir / f"{bench_name}_{cap.label}"
            out_dir.mkdir(parents=True, exist_ok=True)

            result = compile_chunked(
                tc,
                env,
                ChunkedCompileOptions(
                    input_c=bench_path,
                    energy_config=energy_config,
                    milp_config=cap.config_path,
                    output=out_dir / bench_name,
                    pass_log_level=pass_log_level,
                    device_debug=False,
                    cpu_freq=cpu_freq,
                    opt_level=opt_level,
                    clang_opt_level=clang_opt_level,
                    link=True,
                ),
            )
            return result.elf_file

        run_timing_matrix(
            bench_paths=bench_paths,
            capacitors=capacitors,
            compile_fn=compile_fn,
            output_csv=output_csv,
            csv_header=CSV_HEADER,
            saleae_manager=saleae_manager,
            capture_timeout_seconds=capture_timeout_seconds,
        )
