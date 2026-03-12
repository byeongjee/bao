"""Shared benchmark matrix loop and CSV output.

Replaces the duplicated ~150-line loop in each run_*.sh script with a
single generic driver that iterates over (benchmark x capacitor),
calls a compile function, optionally flashes to a device, parses
statistics, and writes CSV rows.
"""

from __future__ import annotations

import csv
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from ..env import ProjectEnv
from ..output_parser import (
    NvmCounters,
    PassStatistics,
    detect_infeasibility,
    has_pass_statistics,
    nvm_counters_to_labels,
    parse_nvm_output,
    parse_pass_output,
)
from ..runner import CompilationError
from ..toolchain import Toolchain
from .config import CapacitorConfig


@dataclass
class BenchmarkRow:
    """A single row of benchmark results for CSV output."""

    benchmark: str  # e.g., "crc-1uF"
    capacitor: str
    status: str  # "ok", "infeasible", "failed"
    fields: dict[str, str | int | None] = field(default_factory=dict)


def check_device_available() -> bool:
    """Check if an MSP430 device is connected.

    Runs ``mspdebug tilib 'exit'`` with a 3-second timeout.
    """
    try:
        subprocess.run(
            ["mspdebug", "tilib", "exit"],
            capture_output=True,
            timeout=3,
        )
        return True
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return False


# Type alias for the row-builder callback.
#
# Signature:
#   (bench_name, cap_label, stats, nvm, compile_output) -> dict[str, str|int|None]
RowBuilder = Callable[
    [str, str, PassStatistics, NvmCounters | None, str],
    dict[str, str | int | None],
]

# Type alias for the per-(benchmark, capacitor) compile function.
#
# Signature:
#   (bench_path, capacitor) -> (output_dir, compile_output_text)
CompileFn = Callable[[Path, CapacitorConfig], tuple[Path, str]]


def run_benchmark_matrix(
    env: ProjectEnv,
    tc: Toolchain,
    benchmarks: list[Path],
    capacitors: list[CapacitorConfig],
    compile_fn: CompileFn,
    output_csv: Path,
    *,
    nvm_symbols: list[str] | None = None,
    debug_counters: bool = False,
    verbose: bool = False,
    csv_header: list[str],
    row_builder: RowBuilder,
) -> None:
    """Run compile + optional flash/NVM-read across benchmark x capacitor matrix.

    For each ``(benchmark, capacitor)``:

    1. Call *compile_fn(benchmark, capacitor)* -> ``(output_dir, pass_output)``
    2. If *debug_counters* and device available: ``flash_run_and_read``
    3. Parse stats, detect infeasibility
    4. Call *row_builder* to get CSV fields
    5. Write to CSV

    Prints progress like ``[1/12] Running crc-1uF ...`` and a summary at
    the end.
    """
    # Lazily check device only when debug counters are requested
    has_device = False
    if debug_counters:
        has_device = check_device_available()
        if has_device:
            print("MSP430 device detected -- will flash and read NVM counters.")
        else:
            print(
                "Warning: No MSP430 device detected -- "
                "skipping NVM readback (runtime counters will be 0)."
            )

    # Open CSV and write header
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(output_csv, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(csv_header)

        total = len(benchmarks) * len(capacitors)
        count = 0

        for bench_path in benchmarks:
            bench_name = bench_path.stem

            for cap in capacitors:
                count += 1
                row_name = f"{bench_name}-{cap.label}"
                print(f"[{count}/{total}] Running {row_name} ...")

                # ----- Compile -----
                try:
                    output_dir, compile_output = compile_fn(bench_path, cap)
                except CompilationError as exc:
                    compile_output = exc.result.output if exc.result else ""
                    output_dir = None

                # ----- Flash + NVM read -----
                nvm: NvmCounters | None = None
                if (
                    has_device
                    and output_dir is not None
                    and nvm_symbols
                ):
                    elf = output_dir / f"{bench_name}.elf"
                    if elf.is_file():
                        nvm = _flash_and_read(tc, elf, nvm_symbols)

                # ----- Merge compile output + NVM labels -----
                full_output = compile_output
                if nvm is not None:
                    nvm_labels = nvm_counters_to_labels(nvm)
                    full_output = f"{compile_output}\n{nvm_labels}"

                if verbose:
                    print(full_output)

                # ----- Check infeasibility -----
                infeasible_reason = detect_infeasibility(full_output)
                if infeasible_reason is not None:
                    print(f"  INFEASIBLE ({infeasible_reason})")
                    row = BenchmarkRow(
                        benchmark=row_name,
                        capacitor=cap.label,
                        status="infeasible",
                    )
                    write_csv_row(writer, row, csv_header)
                    continue

                # ----- Check for compilation failure -----
                if not has_pass_statistics(full_output):
                    print("  FAILED (compilation error)")
                    row = BenchmarkRow(
                        benchmark=row_name,
                        capacitor=cap.label,
                        status="failed",
                    )
                    write_csv_row(writer, row, csv_header)
                    continue

                # ----- Parse stats and build row -----
                stats = parse_pass_output(full_output)
                row_fields = row_builder(
                    bench_name, cap.label, stats, nvm, full_output
                )

                row = BenchmarkRow(
                    benchmark=row_name,
                    capacitor=cap.label,
                    status="ok",
                    fields=row_fields,
                )
                write_csv_row(writer, row, csv_header)

                # Print a brief summary
                _print_ok_summary(row_fields)

    # ----- Final summary -----
    print()
    print("==========================================")
    print(f"Results written to: {output_csv}")
    print("==========================================")
    print()
    _print_csv_table(output_csv)


def write_csv_row(
    writer: csv.writer,  # type: ignore[type-arg]
    row: BenchmarkRow,
    header: list[str],
) -> None:
    """Write a benchmark row to CSV.

    For ``"infeasible"`` or ``"failed"`` rows the fields dict is typically
    empty, producing blank cells for all data columns.
    """
    values: list[str] = []
    for col in header:
        if col == "benchmark":
            values.append(row.benchmark)
        elif col == "capacitor":
            values.append(row.capacitor)
        elif col == "status":
            values.append(row.status)
        else:
            val = row.fields.get(col)
            values.append("" if val is None else str(val))
    writer.writerow(values)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _flash_and_read(tc: Toolchain, elf: Path, symbols: list[str]) -> NvmCounters | None:
    """Flash an ELF, run it, and read NVM counters.

    Returns ``None`` on any error so the caller can continue gracefully.
    """
    try:
        from ..device.flash import flash_run_and_read

        result = flash_run_and_read(tc, elf, 30, symbols)
        nvm_text = "\n".join(f"{k}={v}" for k, v in result.items())
        return parse_nvm_output(nvm_text)
    except Exception:
        return None


def _print_ok_summary(fields: dict[str, str | int | None]) -> None:
    """Print a brief OK summary from row fields."""
    parts: list[str] = []
    for key in ("regions", "region_boundaries", "boundary_checks"):
        val = fields.get(key)
        if val is not None:
            parts.append(f"{val} {key.replace('_', ' ')}")
    for key in ("optimal_solution",):
        val = fields.get(key)
        if val is not None:
            parts.append(str(val))
    summary = ", ".join(parts) if parts else "done"
    print(f"  OK ({summary})")


def _print_csv_table(csv_path: Path) -> None:
    """Print the CSV as a formatted table using ``column -t``."""
    try:
        subprocess.run(
            ["column", "-t", "-s,", str(csv_path)],
            check=False,
        )
    except FileNotFoundError:
        # column not available; just cat it
        print(csv_path.read_text())
