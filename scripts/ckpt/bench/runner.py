"""Shared benchmark matrix loop and CSV output.

Replaces the duplicated ~150-line loop in each run_*.sh script with a
single generic driver that iterates over (benchmark x capacitor),
calls a compile function, optionally flashes to a device, parses
statistics, and writes CSV rows.
"""

from __future__ import annotations

import csv
import json as _json
import logging
import re
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Callable

if TYPE_CHECKING:
    from saleae.automation import Manager

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

logger = logging.getLogger(__name__)

_FLASH_TIMEOUT = 30              # seconds
_AFTER_TRIGGER_SECONDS = 1.0     # seconds to record after falling edge
_POST_CAPTURE_SETTLE_SECONDS = 2.0

# CSV columns that can only be filled by running the linked ELF on a device
# (Saleae timing + NVM counter readback). Compile-only CSVs omit these, and
# device-less bench runs leave them blank.
RUNTIME_CSV_FIELDS: frozenset[str] = frozenset({
    "execution_time_us",
    "result",
    "runtime_region_boundary_calls",
    "runtime_debug_save_reg_calls",
    "runtime_debug_save_vreg_calls",
    "runtime_debug_restore_reg_calls",
    "runtime_debug_restore_vreg_calls",
    "runtime_debug_store_mem_calls",
    "runtime_debug_restore_mem_calls",
})


def static_csv_header(header: list[str]) -> list[str]:
    """Return *header* with the device-only runtime columns removed."""
    return [col for col in header if col not in RUNTIME_CSV_FIELDS]


def build_common_fields(
    stats: PassStatistics,
    profiling_time_ms: int,
    result_val: str,
) -> dict[str, str | int | None]:
    """Build the CSV fields shared by every algorithm from compile-time stats."""
    return {
        "basic_blocks": stats.basic_blocks or 0,
        "edges": stats.edges or 0,
        "abstract_cfg_blocks": stats.abstract_cfg_blocks or stats.abstract_cfg_size or 0,
        "abstract_cfg_edges": stats.abstract_cfg_edges or 0,
        "region_boundaries": stats.region_boundaries or 0,
        "compilation_time_ms": stats.compilation_time_ms or 0,
        "peak_rss_kb": stats.peak_rss_kb or 0,
        "profiling_time_ms": profiling_time_ms,
        "result": result_val,
    }


@dataclass
class BenchmarkRow:
    """A single row of benchmark results for CSV output."""

    benchmark: str  # e.g., "crc-1uF"
    capacitor: str
    status: str  # "ok", "infeasible", "failed"
    fields: dict[str, str | int | None] = field(default_factory=dict)


@dataclass
class CompileResult:
    """Result from a per-(benchmark, capacitor) compilation."""

    out_dir: Path
    pass_output: str
    stats_json: Path | None
    profiling_time_ms: int


def nvm_counter(nvm: NvmCounters | None, attr: str) -> int:
    """Safely read an NVM counter attribute, defaulting to 0."""
    if nvm is None:
        return 0
    val = getattr(nvm, attr, None)
    return val if val is not None else 0


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
#   (bench_path, capacitor) -> CompileResult
CompileFn = Callable[[Path, CapacitorConfig], CompileResult]



_ENERGY_PARAMS_RE = re.compile(
    r"--- Energy parameters.*?---\n"
    r"\s*Required \(\d+ keys?\):(.*)\n"
    r"\s*Missing  \(\d+ keys?\):(.*)\n",
)


def accumulate_keys_to_file(keys: set[str], file_path: Path) -> None:
    """Merge *keys* with any existing keys in *file_path* and write back.

    The file is a single line of comma-separated unique identifiers, sorted.
    """
    existing: set[str] = set()
    if file_path.is_file():
        text = file_path.read_text().strip()
        if text:
            for k in text.split(","):
                k = k.strip()
                if k:
                    existing.add(k)
    merged = sorted(existing | keys)
    file_path.parent.mkdir(parents=True, exist_ok=True)
    file_path.write_text(",".join(merged) + "\n")


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


def run_benchmark_matrix(
    env: ProjectEnv,
    tc: Toolchain,
    benchmarks: list[Path],
    capacitors: list[CapacitorConfig],
    compile_fn: CompileFn,
    output_csv: Path,
    *,
    nvm_symbols: list[str] | None = None,
    device_debug: bool,
    csv_header: list[str],
    row_builder: RowBuilder,
    saleae_manager: Manager | None,
    accumulate_keys_file: Path | None,
) -> None:
    """Run compile + Saleae timing + optional NVM-read across benchmark x capacitor matrix.

    For each ``(benchmark, capacitor)``:

    1. Call *compile_fn(benchmark, capacitor)* -> ``(output_dir, pass_output)``
    2. Flash ELF and measure execution time via Saleae GPIO capture
    3. If *device_debug*: read NVM counters from halted device
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

            for cap in capacitors:
                count += 1
                row_name = f"{bench_name}-{cap.label}"
                logger.info("[%d/%d] Running %s ...", count, total, row_name)

                # ----- Compile -----
                had_compilation_error = False
                compile_result_profiling_ms: int = 0
                stats_json_path: Path | None = None
                try:
                    compile_result = compile_fn(bench_path, cap)
                    output_dir: Path | None = compile_result.out_dir
                    compile_output: str = compile_result.pass_output
                    stats_json_path = compile_result.stats_json
                    compile_result_profiling_ms = compile_result.profiling_time_ms
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

                # ----- Saleae timing + NVM read (only with a device) -----
                nvm: NvmCounters | None = None
                execution_time_us: float | None = None
                if output_dir is not None and saleae_manager is not None:
                    elf = output_dir / f"{bench_name}.elf"
                    if elf.is_file():
                        try:
                            from ..device.saleae import saleae_run
                            from ..device.flash import read_nvm

                            execution_time_us = saleae_run(
                                elf, saleae_manager,
                                _FLASH_TIMEOUT, _AFTER_TRIGGER_SECONDS,
                            )

                            if device_debug and nvm_symbols:
                                time.sleep(_POST_CAPTURE_SETTLE_SECONDS)
                                nvm_dict = read_nvm(
                                    tc, elf, _FLASH_TIMEOUT, nvm_symbols,
                                )
                                nvm_text = "\n".join(
                                    f"{k}={v}" for k, v in nvm_dict.items()
                                )
                                nvm = parse_nvm_output(nvm_text)
                        except DeviceError as exc:
                            logger.error("  DEVICE ERROR: %s", exc)

                # ----- Merge compile output + NVM labels -----
                full_output = compile_output
                if nvm is not None:
                    nvm_labels = nvm_counters_to_labels(nvm)
                    full_output = f"{compile_output}\n{nvm_labels}"

                logger.debug("Full output:\n%s", full_output)

                # ----- Parse stats (prefer JSON, fall back to text) -----
                stats: PassStatistics | None = None
                row_status = "ok"
                if stats_json_data is not None:
                    stats, json_feasible, json_reason = load_stats_json(stats_json_data)
                    if not json_feasible:
                        logger.error("  INFEASIBLE (%s)", json_reason)
                        row_status = "infeasible"
                else:
                    # ----- Check infeasibility (text fallback) -----
                    infeasible_reason = detect_infeasibility(full_output)
                    if infeasible_reason is not None:
                        logger.error("  INFEASIBLE (%s)", infeasible_reason)
                        row_status = "infeasible"
                        if has_pass_statistics(full_output):
                            stats = parse_pass_output(full_output)
                        else:
                            row = BenchmarkRow(
                                benchmark=row_name,
                                capacitor=cap.label,
                                status="infeasible",
                            )
                            write_csv_row(writer, row, csv_header)
                            continue

                    # ----- Check for compilation failure -----
                    if stats is None and not has_pass_statistics(full_output):
                        logger.error("  FAILED (compilation error)")
                        row = BenchmarkRow(
                            benchmark=row_name,
                            capacitor=cap.label,
                            status="failed",
                        )
                        write_csv_row(writer, row, csv_header)
                        continue

                    if stats is None:
                        stats = parse_pass_output(full_output)

                # ----- Build common fields -----
                result_val = ""
                if nvm is not None and nvm.result is not None:
                    result_val = str(nvm.result)
                else:
                    result_val = extract_stat(full_output, "RESULT") or ""
                common_fields = build_common_fields(
                    stats, compile_result_profiling_ms, result_val
                )

                # ----- Build row (algorithm-specific fields on top) -----
                row_fields = common_fields
                row_fields.update(row_builder(
                    bench_name, cap.label, stats, nvm, full_output
                ))
                if execution_time_us is not None:
                    row_fields["execution_time_us"] = str(round(execution_time_us, 2))

                row = BenchmarkRow(
                    benchmark=row_name,
                    capacitor=cap.label,
                    status="link_failed" if had_compilation_error and row_status == "ok" else row_status,
                    fields=row_fields,
                )
                write_csv_row(writer, row, csv_header)

                # Print detailed summary
                print_benchmark_summary(
                    row.status, row_fields,
                    device_debug=device_debug,
                )

    # ----- Energy parameters summary -----
    if all_required_keys:
        logger.info("")
        logger.info("--- Energy parameters ---")
        logger.info("  Required (%d keys): %s", len(all_required_keys), ", ".join(sorted(all_required_keys)))
        logger.info("  Missing  (%d keys): %s", len(all_missing_keys), ", ".join(sorted(all_missing_keys)))
        if accumulate_keys_file is not None:
            accumulate_keys_to_file(all_required_keys, accumulate_keys_file)

    # ----- Final summary -----
    logger.info("")
    logger.info("==========================================")
    logger.info("Results written to: %s", output_csv)
    logger.info("==========================================")


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


def write_compile_stats_csv(
    output_csv: Path,
    full_header: list[str],
    row_builder: RowBuilder,
    *,
    benchmark: str,
    capacitor: str,
    status: str,
    stats: PassStatistics,
    profiling_time_ms: int,
    pass_output: str,
) -> None:
    """Write a single-row CSV of compile-time stats (no device/runtime columns).

    Uses the algorithm's *full_header* and *row_builder* but drops the
    device-only runtime columns, so the result is a strict subset of the
    columns ``ckpt bench`` would emit for the same algorithm.
    """
    header = static_csv_header(full_header)
    fields = build_common_fields(stats, profiling_time_ms, "")
    fields.update(row_builder(benchmark, capacitor, stats, None, pass_output))
    row = BenchmarkRow(
        benchmark=benchmark,
        capacitor=capacitor,
        status=status,
        fields=fields,
    )
    output_csv.parent.mkdir(parents=True, exist_ok=True)
    with open(output_csv, "w", newline="") as csvfile:
        writer = csv.writer(csvfile)
        writer.writerow(header)
        write_csv_row(writer, row, header)


def print_benchmark_summary(
    status: str,
    fields: dict[str, str | int | None],
    *,
    device_debug: bool,
) -> None:
    """Print a detailed multi-line summary for a benchmark run."""
    label = status.upper() if status != "ok" else "OK"
    if status == "ok":
        logger.info("  %s", label)
    else:
        logger.error("  %s", label)

    def _fmt(key: str, fields: dict[str, str | int | None]) -> str | None:
        val = fields.get(key)
        if val is None or val == "" or val == 0:
            return None
        return str(val)

    def _print_group(title: str, items: list[tuple[str, str | None]]) -> None:
        parts = [f"{name} {val}" for name, val in items if val is not None]
        if parts:
            logger.info("    %s%s", f"{title + ':':<16}", ", ".join(parts))

    # CFG
    _print_group("CFG", [
        ("blocks", _fmt("basic_blocks", fields)),
        ("edges", _fmt("edges", fields)),
        ("abstract blocks", _fmt("abstract_cfg_blocks", fields)),
        ("abstract edges", _fmt("abstract_cfg_edges", fields)),
        ("region boundaries", _fmt("region_boundaries", fields)),
    ])

    # MILP
    milp_items: list[tuple[str, str | None]] = [
        ("mode", _fmt("milp_allocation_mode", fields)),
        ("variables", _fmt("milp_variables", fields)),
        ("constraints", _fmt("milp_constraints", fields)),
        ("solve", f"{fields['milp_solve_time_ms']}ms" if _fmt("milp_solve_time_ms", fields) else None),
    ]
    presolved_vars = _fmt("milp_presolved_variables", fields)
    presolved_constraints = _fmt("milp_presolved_constraints", fields)
    if presolved_vars is not None:
        milp_items.append(("vars after presolve", presolved_vars))
    if presolved_constraints is not None:
        milp_items.append(("constrs after presolve", presolved_constraints))
    optimal = _fmt("optimal_solution", fields)
    if optimal is not None:
        milp_items.append(("optimal", optimal))
    _print_group("MILP", milp_items)

    # Checkpoints
    ckpt_items: list[tuple[str, str | None]] = []
    for key, name in [
        ("enabled_checkpoints", "enabled"),
        ("distributed_checkpoints_inserted", "distributed inserted"),
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

    # Runtime (device debug — show even when 0)
    if device_debug:
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
        logger.info("    %s%s KB peak RSS", "Memory:".ljust(16), rss)

    # Result
    result = fields.get("result")
    if result is not None and result != "":
        logger.info("    %s%s", "Result:".ljust(16), result)
