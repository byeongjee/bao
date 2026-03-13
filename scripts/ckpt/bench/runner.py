"""Shared benchmark matrix loop and CSV output.

Replaces the duplicated ~150-line loop in each run_*.sh script with a
single generic driver that iterates over (benchmark x capacitor),
calls a compile function, optionally flashes to a device, parses
statistics, and writes CSV rows.
"""

from __future__ import annotations

import csv
import json as _json
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

from ..env import ProjectEnv
from ..output_parser import (
    NvmCounters,
    PassStatistics,
    detect_infeasibility,
    extract_stat,
    has_pass_statistics,
    load_stats_json,
    nvm_counters_to_labels,
    parse_nvm_output,
    parse_pass_output,
)
from ..errors import DeviceError
from ..runner import CompilationError
from ..toolchain import Toolchain
from .config import CapacitorConfig

_FLASH_TIMEOUT = 30              # seconds
_AFTER_TRIGGER_SECONDS = 1.0     # seconds to record after falling edge


@dataclass
class BenchmarkRow:
    """A single row of benchmark results for CSV output."""

    benchmark: str  # e.g., "crc-1uF"
    capacitor: str
    status: str  # "ok", "infeasible", "failed"
    fields: dict[str, str | int | None] = field(default_factory=dict)


def nvm_counter(nvm: NvmCounters | None, attr: str) -> int:
    """Safely read an NVM counter attribute, defaulting to 0."""
    if nvm is None:
        return 0
    val = getattr(nvm, attr, None)
    return val if val is not None else 0


def build_base_fields(
    stats: PassStatistics, full_output: str,
    nvm: NvmCounters | None,
) -> dict[str, str | int | None]:
    """Build the common stats fields shared by all algorithms."""
    result = ""
    if nvm is not None and nvm.result is not None:
        result = str(nvm.result)
    else:
        result = extract_stat(full_output, "RESULT") or ""
    return {
        "basic_blocks": stats.basic_blocks or 0,
        "edges": stats.edges or 0,
        "regions": stats.regions or 0,
        "compilation_time_ms": stats.compilation_time_ms or 0,
        "peak_rss_kb": stats.peak_rss_kb or 0,
        "result": result,
    }


def check_device_available() -> bool:
    """Check if an MSP430 device is connected.

    Runs ``mspdebug tilib 'exit'`` with a 10-second timeout.
    """
    try:
        result = subprocess.run(
            ["mspdebug", "tilib", "exit"],
            capture_output=True,
            timeout=10,
        )
        return result.returncode == 0
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
CompileFn = Callable[[Path, CapacitorConfig], tuple[Path, str, Path | None]]

# Type alias for the optional per-benchmark pre-hook (e.g. trace collection).
PreBenchmarkFn = Callable[[Path], None]


_ENERGY_PARAMS_RE = re.compile(
    r"--- Energy parameters.*?---\n"
    r"\s*Required \(\d+ keys?\):(.*)\n"
    r"\s*Missing  \(\d+ keys?\):(.*)\n",
)


def extract_energy_params(text: str) -> tuple[list[str], list[str]] | None:
    """Extract required and missing energy parameter keys from pass output.

    Merges keys from all ``--- Energy parameters ---`` sections found in
    *text* (there may be multiple from pre/post assembly or per-function).

    Returns ``(required, missing)`` or ``None`` if no section was found.
    """
    required: set[str] = set()
    missing: set[str] = set()
    for match in _ENERGY_PARAMS_RE.finditer(text):
        for k in match.group(1).split(","):
            k = k.strip()
            if k:
                required.add(k)
        for k in match.group(2).split(","):
            k = k.strip()
            if k:
                missing.add(k)
    if not required and not missing:
        return None
    return sorted(required), sorted(missing)


class BenchmarkSkipped(Exception):
    """Raised by pre_benchmark to skip all capacitors for a benchmark."""

    def __init__(self, status: str):
        self.status = status


def run_benchmark_matrix(
    env: ProjectEnv,
    tc: Toolchain,
    benchmarks: list[Path],
    capacitors: list[CapacitorConfig],
    compile_fn: CompileFn,
    output_csv: Path,
    *,
    nvm_symbols: list[str] | None = None,
    debug_counters: bool,
    verbose: bool,
    csv_header: list[str],
    row_builder: RowBuilder,
    pre_benchmark: PreBenchmarkFn | None = None,
    saleae_manager: object,
) -> None:
    """Run compile + Saleae timing + optional NVM-read across benchmark x capacitor matrix.

    For each ``(benchmark, capacitor)``:

    1. Call *compile_fn(benchmark, capacitor)* -> ``(output_dir, pass_output)``
    2. Flash ELF and measure execution time via Saleae GPIO capture
    3. If *debug_counters*: read NVM counters from halted device
    4. Parse stats, detect infeasibility
    5. Call *row_builder* to get CSV fields, inject ``execution_time_us``
    6. Write to CSV

    Prints progress like ``[1/12] Running crc-1uF ...`` and a summary at
    the end.
    """
    # Open CSV and write header
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(output_csv, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(csv_header)

        total = len(benchmarks) * len(capacitors)
        count = 0
        all_required_keys: set[str] = set()
        all_missing_keys: set[str] = set()

        for bench_path in benchmarks:
            bench_name = bench_path.stem

            # Per-benchmark setup (e.g. trace collection for SCHEMATIC)
            if pre_benchmark is not None:
                try:
                    pre_benchmark(bench_path)
                except BenchmarkSkipped as exc:
                    for cap in capacitors:
                        count += 1
                        row = BenchmarkRow(
                            benchmark=f"{bench_name}-{cap.label}",
                            capacitor=cap.label,
                            status=exc.status,
                        )
                        write_csv_row(writer, row, csv_header)
                    continue

            for cap in capacitors:
                count += 1
                row_name = f"{bench_name}-{cap.label}"
                print(f"[{count}/{total}] Running {row_name} ...")

                # ----- Compile -----
                had_compilation_error = False
                stats_json_path: Path | None = None
                try:
                    output_dir, compile_output, stats_json_path = compile_fn(bench_path, cap)
                except CompilationError as exc:
                    compile_output = exc.pass_output or (exc.result.output if exc.result else "")
                    output_dir = None
                    stats_json_path = getattr(exc, "stats_json", None)
                    had_compilation_error = True

                # ----- Load JSON sidecar once (if available) -----
                stats_json_data: dict | None = None
                if stats_json_path is not None and stats_json_path.is_file():
                    with open(stats_json_path) as _f:
                        stats_json_data = _json.load(_f)

                # ----- Collect energy params -----
                ep = extract_energy_params(compile_output)
                if ep is not None:
                    all_required_keys.update(ep[0])
                    all_missing_keys.update(ep[1])
                elif stats_json_data is not None:
                    req = stats_json_data.get("required_energy_keys", [])
                    miss = stats_json_data.get("missing_energy_keys", [])
                    if req or miss:
                        all_required_keys.update(req)
                        all_missing_keys.update(miss)

                # ----- Saleae timing + NVM read -----
                nvm: NvmCounters | None = None
                execution_time_us: float | None = None
                if output_dir is not None:
                    elf = output_dir / f"{bench_name}.elf"
                    if elf.is_file():
                        try:
                            from ..device.saleae import saleae_run
                            from ..device.flash import read_nvm

                            execution_time_us = saleae_run(
                                tc, elf, saleae_manager,
                                _FLASH_TIMEOUT, _AFTER_TRIGGER_SECONDS,
                            )

                            if debug_counters and nvm_symbols:
                                nvm_dict = read_nvm(
                                    tc, elf, _FLASH_TIMEOUT, nvm_symbols,
                                )
                                nvm_text = "\n".join(
                                    f"{k}={v}" for k, v in nvm_dict.items()
                                )
                                nvm = parse_nvm_output(nvm_text)
                        except DeviceError as exc:
                            print(f"  DEVICE ERROR: {exc}")

                # ----- Merge compile output + NVM labels -----
                full_output = compile_output
                if nvm is not None:
                    nvm_labels = nvm_counters_to_labels(nvm)
                    full_output = f"{compile_output}\n{nvm_labels}"

                if verbose:
                    print(full_output)

                # ----- Parse stats (prefer JSON, fall back to text) -----
                stats: PassStatistics | None = None
                if stats_json_data is not None:
                    stats, json_feasible, json_reason = load_stats_json(stats_json_data)
                    if not json_feasible:
                        print(f"  INFEASIBLE ({json_reason})")
                        row = BenchmarkRow(
                            benchmark=row_name,
                            capacitor=cap.label,
                            status="infeasible",
                        )
                        write_csv_row(writer, row, csv_header)
                        continue
                else:
                    # ----- Check infeasibility (text fallback) -----
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

                    stats = parse_pass_output(full_output)

                # ----- Build row -----
                row_fields = row_builder(
                    bench_name, cap.label, stats, nvm, full_output
                )
                if execution_time_us is not None:
                    row_fields["execution_time_us"] = str(round(execution_time_us, 2))

                row = BenchmarkRow(
                    benchmark=row_name,
                    capacitor=cap.label,
                    status="link_failed" if had_compilation_error else "ok",
                    fields=row_fields,
                )
                write_csv_row(writer, row, csv_header)

                # Print detailed summary
                print_benchmark_summary(
                    row.status, row_fields,
                    debug_counters=debug_counters,
                )

    # ----- Energy parameters summary -----
    if all_required_keys:
        print()
        print("--- Energy parameters ---")
        print(f"  Required ({len(all_required_keys)} keys): {', '.join(sorted(all_required_keys))}")
        print(f"  Missing  ({len(all_missing_keys)} keys): {', '.join(sorted(all_missing_keys))}")

    # ----- Final summary -----
    print()
    print("==========================================")
    print(f"Results written to: {output_csv}")
    print("==========================================")


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


def print_benchmark_summary(
    status: str,
    fields: dict[str, str | int | None],
    *,
    debug_counters: bool,
) -> None:
    """Print a detailed multi-line summary for a benchmark run."""
    label = status.upper() if status != "ok" else "OK"
    print(f"  {label}")

    def _fmt(key: str, fields: dict[str, str | int | None]) -> str | None:
        val = fields.get(key)
        if val is None or val == "" or val == 0:
            return None
        return str(val)

    def _print_group(title: str, items: list[tuple[str, str | None]]) -> None:
        parts = [f"{name} {val}" for name, val in items if val is not None]
        if parts:
            print(f"    {title + ':':<16}{', '.join(parts)}")

    # CFG
    _print_group("CFG", [
        ("blocks", _fmt("basic_blocks", fields)),
        ("edges", _fmt("edges", fields)),
        ("regions", _fmt("regions", fields)),
    ])

    # MILP
    milp_items: list[tuple[str, str | None]] = [
        ("variables", _fmt("milp_variables", fields)),
        ("constraints", _fmt("milp_constraints", fields)),
        ("solve", f"{fields['milp_solve_time_ms']}ms" if _fmt("milp_solve_time_ms", fields) else None),
    ]
    optimal = _fmt("optimal_solution", fields)
    if optimal is not None:
        milp_items.append(("optimal", optimal))
    _print_group("MILP", milp_items)

    # Checkpoints
    ckpt_items: list[tuple[str, str | None]] = []
    for key, name in [
        ("region_boundaries", "region boundaries"),
        ("region_boundaries_inserted", "boundaries inserted"),
        ("enabled_checkpoints", "enabled"),
        ("distributed_checkpoints_inserted", "distributed inserted"),
        ("boundary_checks", "boundary checks"),
        ("runtime_calls_inserted", "runtime calls"),
    ]:
        val = _fmt(key, fields)
        if val is not None:
            ckpt_items.append((name, val))
    _print_group("Checkpoints", ckpt_items)

    # Analysis
    _print_group("Analysis", [
        ("candidate globals", _fmt("candidate_globals", fields)),
        ("loop decisions", _fmt("loop_decisions", fields)),
        ("paths analyzed", _fmt("paths_analyzed", fields)),
    ])

    # Runtime (debug counters — show even when 0)
    if debug_counters:
        runtime_items: list[tuple[str, str | None]] = []
        for key, name in [
            ("runtime_region_boundary_calls", "region boundaries"),
            ("runtime_debug_save_reg_calls", "reg saves"),
            ("runtime_debug_save_vreg_calls", "vreg saves"),
            ("runtime_debug_restore_reg_calls", "reg restores"),
            ("runtime_debug_restore_vreg_calls", "vreg restores"),
            ("runtime_debug_store_mem_calls", "mem stores"),
            ("runtime_debug_restore_mem_calls", "mem restores"),
        ]:
            if key in fields:
                runtime_items.append((name, str(fields[key])))
        _print_group("Runtime", runtime_items)

    # Timing
    timing_items: list[tuple[str, str | None]] = []
    for key, name, unit in [
        ("compilation_time_ms", "compile", "ms"),
        ("profiling_time_ms", "profiling", "ms"),
        ("execution_time_us", "execution", "us"),
    ]:
        raw = _fmt(key, fields)
        if raw is not None:
            timing_items.append((name, f"{raw}{unit}"))
    _print_group("Timing", timing_items)

    # Memory
    rss = _fmt("peak_rss_kb", fields)
    if rss is not None:
        print(f"    {'Memory:':<16}{rss} KB peak RSS")

    # Result
    result = fields.get("result")
    if result is not None and result != "":
        print(f"    {'Result:':<16}{result}")
