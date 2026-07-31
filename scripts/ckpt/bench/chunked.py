"""Chunking-only benchmark runner — chunked baseline execution time.

Compiles each benchmark with loop chunking applied but no checkpoint
instrumentation, flashes to an MSP430 device, and measures execution time
via Saleae GPIO capture. Chunk sizes depend on the energy budget, so
benchmarks run per capacitor size like the MILP runs.
"""

from __future__ import annotations

import csv
import logging
import time
from pathlib import Path

from ..compile.chunked import ChunkedCompileOptions, compile_chunked
from ..env import ProjectEnv
from ..errors import CkptError, ConfigError, DeviceError
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import default_energy_config, discover_benchmarks, discover_capacitors
from .runner import check_device_available

logger = logging.getLogger(__name__)

CSV_HEADER: list[str] = [
    "benchmark",
    "capacitor",
    "status",
    "compilation_time_ms",
    "execution_time_us",
]

_FLASH_TIMEOUT = 30
_AFTER_TRIGGER_SECONDS = 1.0


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

    saleae_manager = None
    if check_device_available():
        from ..device.saleae import discover_saleae

        saleae_manager = discover_saleae()
    else:
        logger.warning(
            "No MSP430 device detected; running compile-only (no flash or "
            "timing). The execution_time_us column will be left blank."
        )

    try:
        output_csv.parent.mkdir(parents=True, exist_ok=True)

        with (
            compilation_workdir(prefix="chunked_bench_") as workdir,
            open(output_csv, "w", newline="") as csvfile,
        ):
            writer = csv.writer(csvfile)
            writer.writerow(CSV_HEADER)

            total = len(bench_paths) * len(capacitors)
            i = 0
            for bench_path in bench_paths:
                bench_name = bench_path.stem
                for cap in capacitors:
                    i += 1
                    logger.info(
                        "[%d/%d] Running %s (%s) ...", i, total, bench_name, cap.label
                    )

                    out_dir = workdir / f"{bench_name}_{cap.label}"
                    out_dir.mkdir(parents=True, exist_ok=True)

                    # Compile
                    try:
                        t0 = time.monotonic()
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
                        compilation_time_ms = int((time.monotonic() - t0) * 1000)
                    except CkptError as exc:
                        logger.error("  FAILED (compilation): %s", exc)
                        writer.writerow([bench_name, cap.label, "failed", "", ""])
                        continue

                    if result.elf_file is None or not result.elf_file.exists():
                        logger.error("  FAILED (no ELF produced)")
                        writer.writerow(
                            [
                                bench_name,
                                cap.label,
                                "failed",
                                str(compilation_time_ms),
                                "",
                            ]
                        )
                        continue

                    # Compile-only mode (no device): record compile time, skip timing.
                    if saleae_manager is None:
                        logger.info(
                            "  OK (compile-only)  compilation_time=%dms",
                            compilation_time_ms,
                        )
                        writer.writerow(
                            [bench_name, cap.label, "ok", str(compilation_time_ms), ""]
                        )
                        continue

                    # Flash + measure
                    try:
                        from ..device.saleae import saleae_run

                        execution_time_us = saleae_run(
                            result.elf_file,
                            saleae_manager,
                            _FLASH_TIMEOUT,
                            _AFTER_TRIGGER_SECONDS,
                            capture_timeout_seconds,
                        )
                        logger.info(
                            "  OK  compilation_time=%dms execution_time=%.2fus",
                            compilation_time_ms,
                            execution_time_us,
                        )
                        writer.writerow(
                            [
                                bench_name,
                                cap.label,
                                "ok",
                                str(compilation_time_ms),
                                str(round(execution_time_us, 2)),
                            ]
                        )
                    except DeviceError as exc:
                        logger.error("  DEVICE ERROR: %s", exc)
                        writer.writerow(
                            [
                                bench_name,
                                cap.label,
                                "device_error",
                                str(compilation_time_ms),
                                "",
                            ]
                        )
    finally:
        if saleae_manager is not None:
            saleae_manager.close()

    logger.info("")
    logger.info("==========================================")
    logger.info("Results written to: %s", output_csv)
    logger.info("==========================================")
