"""Uninstrumented benchmark runner — baseline execution time measurement.

Compiles each benchmark without checkpoint insertion, flashes to an MSP430
device, and measures execution time via Saleae GPIO capture.
"""

from __future__ import annotations

import logging
from pathlib import Path

from ..compile.uninstrumented import (
    UninstrumentedCompileOptions,
    compile_uninstrumented,
)
from ..env import ProjectEnv
from ..errors import ConfigError
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import CapacitorConfig, discover_benchmarks
from .runner import (
    TIMING_COMPILE_ONLY_WARNING,
    optional_saleae,
    run_timing_matrix,
)

logger = logging.getLogger(__name__)

CSV_HEADER: list[str] = [
    "benchmark",
    "status",
    "compilation_time_ms",
    "execution_time_us",
    "run_attempts",
]


def run_uninstrumented_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    output_csv: Path | None,
    capture_timeout_seconds: float,
    cpu_freq: int,
    algorithm_label: str,
    clang_opt_level: int,
    opt_level: int,
) -> None:
    """Compile and measure uninstrumented baselines for all benchmarks.

    For each benchmark:
      1. Compile without checkpoint insertion
      2. Flash to device
      3. Measure execution time via Saleae GPIO capture
      4. Write results to CSV
    """
    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        raise ConfigError("No benchmarks to run")

    if output_csv is None:
        output_csv = (
            env.project_dir / "benchmarks" / f"{algorithm_label}_benchmark_summary.csv"
        )

    with (
        optional_saleae(TIMING_COMPILE_ONLY_WARNING) as saleae_manager,
        compilation_workdir(prefix=f"{algorithm_label}_bench_") as workdir,
    ):

        def compile_fn(bench_path: Path, _cap: CapacitorConfig | None) -> Path | None:
            bench_name = bench_path.stem
            out_dir = workdir / bench_name
            out_dir.mkdir(parents=True, exist_ok=True)

            result = compile_uninstrumented(
                tc,
                env,
                UninstrumentedCompileOptions(
                    input_c=bench_path,
                    output=out_dir / bench_name,
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
            capacitors=None,
            compile_fn=compile_fn,
            output_csv=output_csv,
            csv_header=CSV_HEADER,
            saleae_manager=saleae_manager,
            capture_timeout_seconds=capture_timeout_seconds,
        )
