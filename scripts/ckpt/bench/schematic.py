"""SCHEMATIC benchmark runner -- replaces run_schematic.sh.

SCHEMATIC uses a two-phase approach per benchmark:

1. **Trace collection** -- run once per benchmark with ``--trace-only``
   and ``-Oc 0`` using a dummy (10uF) config.
2. **Per-capacitor compilation** -- compile with ``--trace <collected_trace>``
   ``--link`` ``--add-debug-markers`` for each capacitor size.

Optionally flashes to an MSP430 device and writes a CSV summary.
"""

from __future__ import annotations

import sys
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
from ..runner import CompilationError
from ..tempdir import compilation_workdir
from ..toolchain import Toolchain
from .config import (
    default_energy_config,
    discover_benchmarks,
    discover_capacitors,
)
from .runner import BenchmarkRow, write_csv_row

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

    # Runtime counters from NVM readback (default to 0)
    runtime_boundary = nvm.region_boundary if nvm and nvm.region_boundary is not None else 0
    runtime_save_reg = nvm.save_reg if nvm and nvm.save_reg is not None else 0
    runtime_restore_reg = nvm.restore_reg if nvm and nvm.restore_reg is not None else 0
    runtime_store_mem = nvm.store_mem if nvm and nvm.store_mem is not None else 0
    runtime_restore_mem = nvm.restore_mem if nvm and nvm.restore_mem is not None else 0

    # Computation result (semantic correctness)
    bench_result = extract_stat(full_output, "RESULT")

    return {
        "basic_blocks": stats.basic_blocks or 0,
        "edges": stats.edges or 0,
        "regions": stats.regions or 0,
        "compilation_time_ms": stats.compilation_time_ms or 0,
        "peak_rss_kb": stats.peak_rss_kb or 0,
        "profiling_time_ms": profiling_time_ms,
        "runtime_region_boundary_calls": runtime_boundary,
        "runtime_debug_save_reg_calls": runtime_save_reg,
        "runtime_debug_restore_reg_calls": runtime_restore_reg,
        "runtime_debug_store_mem_calls": runtime_store_mem,
        "runtime_debug_restore_mem_calls": runtime_restore_mem,
        "result": bench_result or "",
        "candidate_globals": stats.candidate_globals or 0,
        "region_boundaries": stats.region_boundaries or 0,
        "enabled_checkpoints": stats.enabled_checkpoints or 0,
        "loop_decisions": stats.loop_decisions or 0,
        "paths_analyzed": stats.paths_analyzed or 0,
        "runtime_calls_inserted": stats.runtime_calls_inserted or 0,
    }


def run_schematic_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None = None,
    caps: list[str] | None = None,
    output_csv: Path | None = None,
    debug_counters: bool = False,
    halt_mode: bool = False,
    verbose: bool = False,
    energy_config: Path | None = None,
    trace_config: Path | None = None,
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
        Enable LPM4 halt at region boundaries.
    verbose:
        Print full compiler output for each benchmark.
    """
    import csv
    import subprocess

    from ..output_parser import (
        detect_infeasibility,
        has_pass_statistics,
        nvm_counters_to_labels,
        parse_nvm_output,
        parse_pass_output,
    )
    from .runner import check_device_available

    bench_paths = discover_benchmarks(env, benchmarks)
    if not bench_paths:
        print("Error: No benchmarks to run", file=sys.stderr)
        raise SystemExit(1)

    capacitors = discover_capacitors(env, "schematic", caps)

    if output_csv is None:
        output_csv = env.project_dir / "benchmarks" / "schematic_benchmark_summary.csv"

    if energy_config is None:
        energy_config = default_energy_config(env, "schematic")

    # Config for trace collection phase (any capacitor works; 10uF by default)
    if trace_config is None:
        trace_config = env.project_dir / "benchmarks" / "config_10uF.json"

    # Check device availability if debug counters requested
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

    output_csv.parent.mkdir(parents=True, exist_ok=True)

    with (
        compilation_workdir(prefix="schematic_bench_") as workdir,
        open(output_csv, "w", newline="") as csvfile,
    ):
        writer = csv.writer(csvfile)
        writer.writerow(_CSV_HEADER)

        total = len(bench_paths) * len(capacitors)
        count = 0

        for bench_path in bench_paths:
            bench_name = bench_path.stem

            # === Phase 1: Collect trace once per benchmark ===
            print()
            print(f"--- Collecting trace for {bench_name} ---")

            trace_json = workdir / f"{bench_name}_trace.json"
            profiling_time_ms = 0

            try:
                trace_opts = SchematicCompileOptions(
                    input_c=bench_path,
                    output=workdir / bench_name,
                    energy_config=energy_config,
                    schematic_config=trace_config,
                    clang_opt_level=0,
                    trace_only=True,
                    verbose=verbose,
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

                # Move trace file to expected location
                produced_trace = workdir / f"{bench_name}_trace.json"
                if not produced_trace.is_file():
                    raise FileNotFoundError(f"Trace file not produced: {produced_trace}")
                trace_json = produced_trace

                print(f"  Trace collected for {bench_name} (profiling: {profiling_time_ms}ms)")

            except (CompilationError, FileNotFoundError, OSError) as exc:
                print(f"  FAILED: trace collection for {bench_name}")
                if verbose:
                    if isinstance(exc, CompilationError) and exc.result:
                        print(exc.result.output[-500:])
                    else:
                        print(str(exc))
                # Write failure rows for all capacitors
                for cap in capacitors:
                    count += 1
                    row = BenchmarkRow(
                        benchmark=f"{bench_name}-{cap.label}",
                        capacitor=cap.label,
                        status="TRACE_FAILED",
                    )
                    write_csv_row(writer, row, _CSV_HEADER)
                continue

            # === Phase 2: Per-capacitor compile + optional flash ===
            for cap in capacitors:
                count += 1
                row_name = f"{bench_name}-{cap.label}"
                print(f"[{count}/{total}] Running {row_name} ...")

                if not cap.config_path.is_file():
                    print(f"  SKIPPED: config not found: {cap.config_path}")
                    row = BenchmarkRow(
                        benchmark=row_name,
                        capacitor=cap.label,
                        status="CONFIG_NOT_FOUND",
                    )
                    write_csv_row(writer, row, _CSV_HEADER)
                    continue

                # Compile with trace
                out_dir = workdir / f"{bench_name}_{cap.label}"
                out_dir.mkdir(parents=True, exist_ok=True)

                try:
                    compile_opts = SchematicCompileOptions(
                        input_c=bench_path,
                        output=out_dir / bench_name,
                        energy_config=energy_config,
                        schematic_config=cap.config_path,
                        clang_opt_level=0,
                        trace_file=trace_json,
                        link=True,
                        add_debug_markers=True,
                        debug_counters=debug_counters,
                        halt_mode=halt_mode,
                        verbose=verbose,
                        extra_includes=[str(env.project_dir / "passes" / "runtime")],
                    )
                    compile_result: SchematicCompileResult = compile_schematic(
                        tc, env, compile_opts
                    )
                    compile_output = compile_result.pass_output
                except CompilationError as exc:
                    compile_output = exc.result.output if exc.result else ""

                if verbose:
                    print(compile_output)

                # Flash + NVM read
                nvm: NvmCounters | None = None
                if has_device:
                    elf = out_dir / f"{bench_name}.elf"
                    if elf.is_file():
                        try:
                            from ..device.flash import flash_run_and_read

                            nvm_dict = flash_run_and_read(
                                tc, elf, 30, _NVM_SYMBOLS
                            )
                            nvm_text = "\n".join(f"{k}={v}" for k, v in nvm_dict.items())
                            nvm = parse_nvm_output(nvm_text)
                        except Exception:
                            nvm = None

                # Merge outputs
                full_output = compile_output
                if nvm is not None:
                    nvm_labels = nvm_counters_to_labels(nvm)
                    full_output = f"{compile_output}\n{nvm_labels}"

                # Check infeasibility
                infeasible_reason = detect_infeasibility(full_output)
                if infeasible_reason is not None:
                    print(f"  INFEASIBLE ({infeasible_reason})")
                    row = BenchmarkRow(
                        benchmark=row_name,
                        capacitor=cap.label,
                        status="infeasible",
                    )
                    write_csv_row(writer, row, _CSV_HEADER)
                    continue

                # Check for compilation failure
                if not has_pass_statistics(full_output):
                    print("  FAILED (SCHEMATIC pass error)")
                    if not verbose:
                        # Show tail of output for debugging
                        lines = full_output.strip().splitlines()
                        for line in lines[-5:]:
                            print(f"    {line}")
                    row = BenchmarkRow(
                        benchmark=row_name,
                        capacitor=cap.label,
                        status="failed",
                    )
                    write_csv_row(writer, row, _CSV_HEADER)
                    continue

                # Parse stats and build row
                stats = parse_pass_output(full_output)
                row_fields = _build_row(
                    bench_name,
                    cap.label,
                    stats,
                    nvm,
                    full_output,
                    profiling_time_ms=profiling_time_ms,
                )

                row = BenchmarkRow(
                    benchmark=row_name,
                    capacitor=cap.label,
                    status="ok",
                    fields=row_fields,
                )
                write_csv_row(writer, row, _CSV_HEADER)

                # Print brief summary
                regions = row_fields.get("regions", 0)
                enabled_ckpts = row_fields.get("enabled_checkpoints", 0)
                runtime_calls = row_fields.get("runtime_calls_inserted", 0)
                rt_boundary = row_fields.get("runtime_region_boundary_calls", 0)
                print(
                    f"  OK ({regions} regions, {enabled_ckpts} checkpoints, "
                    f"{runtime_calls} runtime calls, {rt_boundary} boundaries)"
                )

    # Final summary
    print()
    print("==========================================")
    print(f"Results written to: {output_csv}")
    print("==========================================")
    print()
    try:
        subprocess.run(
            ["column", "-t", "-s,", str(output_csv)],
            check=False,
        )
    except FileNotFoundError:
        print(output_csv.read_text())
