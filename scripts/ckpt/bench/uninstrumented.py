"""Uninstrumented benchmark runner — baseline execution time measurement.

Compiles each benchmark without checkpoint insertion, flashes to an MSP430
device, and measures execution time via Saleae GPIO capture.
"""

from __future__ import annotations

import csv
import logging
import time
from pathlib import Path

from ..compile.uninstrumented import (
    UninstrumentedCompileOptions,
    compile_uninstrumented,
)
from ..env import ProjectEnv
from ..errors import ConfigError, DeviceError
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import discover_benchmarks
from .runner import check_device_available

logger = logging.getLogger(__name__)

_CSV_HEADER: list[str] = [
    "benchmark",
    "status",
    "compilation_time_ms",
    "execution_time_us",
]

_FLASH_TIMEOUT = 30
_AFTER_TRIGGER_SECONDS = 1.0


def run_uninstrumented_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    output_csv: Path | None,
    cpu_freq: int,
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
        output_csv = env.project_dir / "benchmarks" / "uninstrumented_benchmark_summary.csv"

    if not check_device_available():
        raise ConfigError("No MSP430 device detected")

    from ..device.saleae import discover_saleae

    saleae_manager = discover_saleae()

    output_csv.parent.mkdir(parents=True, exist_ok=True)

    with compilation_workdir(prefix="uninstrumented_bench_") as workdir, \
         open(output_csv, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(_CSV_HEADER)

        total = len(bench_paths)
        for i, bench_path in enumerate(bench_paths, 1):
            bench_name = bench_path.stem
            logger.info("[%d/%d] Running %s ...", i, total, bench_name)

            out_dir = workdir / bench_name
            out_dir.mkdir(parents=True, exist_ok=True)

            # Compile
            try:
                t0 = time.monotonic()
                result = compile_uninstrumented(
                    tc, env,
                    UninstrumentedCompileOptions(
                        input_c=bench_path,
                        output=out_dir / bench_name,
                        device_debug=False,
                        cpu_freq=cpu_freq,
                        opt_level=3,
                        clang_opt_level=3,
                        link=True,
                    ),
                )
                compilation_time_ms = int((time.monotonic() - t0) * 1000)
            except Exception as exc:
                logger.error("  FAILED (compilation): %s", exc)
                writer.writerow([bench_name, "failed", "", ""])
                continue

            if result.elf_file is None or not result.elf_file.exists():
                logger.error("  FAILED (no ELF produced)")
                writer.writerow([bench_name, "failed", str(compilation_time_ms), ""])
                continue

            # Flash + measure
            try:
                from ..device.saleae import saleae_run

                execution_time_us = saleae_run(
                    result.elf_file, saleae_manager,
                    _FLASH_TIMEOUT, _AFTER_TRIGGER_SECONDS,
                )
                logger.info("  OK  compilation_time=%dms execution_time=%.2fus",
                            compilation_time_ms, execution_time_us)
                writer.writerow([
                    bench_name, "ok",
                    str(compilation_time_ms),
                    str(round(execution_time_us, 2)),
                ])
            except DeviceError as exc:
                logger.error("  DEVICE ERROR: %s", exc)
                writer.writerow([bench_name, "device_error", str(compilation_time_ms), ""])

    logger.info("")
    logger.info("==========================================")
    logger.info("Results written to: %s", output_csv)
    logger.info("==========================================")
