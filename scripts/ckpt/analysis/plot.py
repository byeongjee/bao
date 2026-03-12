"""Benchmark result plotting.

Absorbs scripts/plot_benchmarks.py. The matplotlib import is optional --
a clear error is raised if it is not installed.
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

ALGORITHMS = {
    "milp": {
        "file": "milp_benchmark_summary.csv",
        "label": "MILP",
        "color": "#2196F3",
    },
    "rockclimb": {
        "file": "rockclimb_benchmark_summary.csv",
        "label": "RockClimb (Machine)",
        "color": "#FF9800",
    },
    "schematic": {
        "file": "schematic_benchmark_summary.csv",
        "label": "SCHEMATIC",
        "color": "#4CAF50",
    },
}

METRICS = {
    "prologue": {
        "column": "runtime_region_prologue_calls",
        "ylabel": "Number of Runtime Region Prologue Calls",
        "title": "Runtime Region Prologue Calls",
    },
    "compilation_time": {
        "column": "compilation_time_ms",
        "ylabel": "Compilation Time (ms)",
        "title": "Compilation Time",
    },
    "profiling_time": {
        "column": "profiling_time_ms",
        "ylabel": "Profiling Time (ms)",
        "title": "Profiling Time",
    },
    "execution_time": {
        "column": "execution_time_ms",
        "ylabel": "Execution Time (ms)",
        "title": "Execution Time",
    },
    "peak_memory": {
        "column": "peak_rss_kb",
        "ylabel": "Peak RSS (KB)",
        "title": "Peak Memory Usage",
    },
    "checkpoint_store_reg_calls": {
        "column": "runtime_checkpoint_store_reg_calls",
        "ylabel": "Number of checkpoint store register calls",
        "title": "Checkpoint Store Register Calls",
    },
    "checkpoint_restore_reg_calls": {
        "column": "runtime_restore_reg_calls",
        "ylabel": "Number of checkpoint restore register calls",
        "title": "Checkpoint Restore Register Calls",
    },
}

_CAP_ORDER = {"1uF": 0, "10uF": 1, "100uF": 2}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _read_csv(filepath: Path) -> list[dict[str, str]]:
    """Read a CSV file and return rows as list of dicts."""
    with filepath.open(newline="") as f:
        return list(csv.DictReader(f))


def _parse_benchmark_name(benchmark: str) -> tuple[str, str]:
    """Split 'program-capacitor' into (program, capacitor)."""
    parts = benchmark.rsplit("-", 1)
    if len(parts) == 2:
        return parts[0], parts[1]
    return benchmark, ""


def _get_value(row: dict[str, str], metric_key: str) -> float | None:
    """Extract a numeric value from a CSV row."""
    col = METRICS[metric_key]["column"]
    val = row.get(col, "")
    if not val:
        return None
    try:
        return float(val)
    except ValueError:
        return None


def _sort_key(label: str) -> tuple[str, int]:
    parts = label.split("\n")
    program = parts[0]
    cap = parts[1].strip("()") if len(parts) > 1 else ""
    return (program, _CAP_ORDER.get(cap, 99))


def _load_data(
    csv_dir: Path,
    algorithms: list[str],
    benchmarks: list[str] | None,
    capacitors: list[str] | None,
    metric_key: str,
) -> tuple[dict[str, dict[str, float]], list[str]]:
    """Load and filter data from CSV files.

    Returns:
        data: dict[algorithm_key] -> dict[benchmark_label] -> value
        benchmark_labels: ordered list of benchmark labels to plot
    """
    data: dict[str, dict[str, float]] = {}
    all_labels: set[str] = set()

    for alg_key in algorithms:
        alg = ALGORITHMS[alg_key]
        filepath = csv_dir / alg["file"]
        if not filepath.exists():
            print(
                f"Warning: {filepath} not found, skipping {alg_key}",
                file=sys.stderr,
            )
            continue

        rows = _read_csv(filepath)
        alg_data: dict[str, float] = {}

        for row in rows:
            bm = row.get("benchmark", "").strip()
            if not bm:
                continue
            program, capacitor = _parse_benchmark_name(bm)

            if benchmarks and program not in benchmarks:
                continue
            if capacitors and capacitor not in capacitors:
                continue

            label = f"{program}\n({capacitor})"
            val = _get_value(row, metric_key)
            if val is not None:
                alg_data[label] = val
                all_labels.add(label)

        data[alg_key] = alg_data

    benchmark_labels = sorted(all_labels, key=_sort_key)
    return data, benchmark_labels


def _normalize_data(
    data: dict[str, dict[str, float]],
    benchmark_labels: list[str],
    baseline_alg: str,
) -> tuple[dict[str, dict[str, float]], list[str]]:
    """Normalize all values relative to the baseline algorithm (baseline = 1.0)."""
    baseline_data = data.get(baseline_alg, {})
    normalized: dict[str, dict[str, float]] = {}
    kept_labels: list[str] = []

    for label in benchmark_labels:
        base_val = baseline_data.get(label)
        if not base_val or base_val == 0:
            continue
        kept_labels.append(label)
        for alg_key, alg_data in data.items():
            if alg_key not in normalized:
                normalized[alg_key] = {}
            val = alg_data.get(label)
            if val is not None:
                normalized[alg_key][label] = val / base_val

    return normalized, kept_labels


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def plot_benchmarks(
    *,
    csv_dir: Path,
    metric: str = "prologue",
    algorithms: list[str] | None = None,
    benchmarks: list[str] | None = None,
    capacitors: list[str] | None = None,
    normalize: str | None = None,
    output_file: Path | None = None,
) -> None:
    """Plot benchmark comparison chart.

    Requires ``matplotlib`` and ``numpy`` (install with
    ``pip install checkpoint-insertion[plot]``).
    """
    try:
        import matplotlib.pyplot as plt  # type: ignore[import-untyped]
        import numpy as np  # type: ignore[import-untyped]
    except ImportError as exc:
        raise ImportError(
            "Plotting requires matplotlib and numpy. "
            "Install with: pip install checkpoint-insertion[plot]"
        ) from exc

    if metric not in METRICS:
        raise ValueError(
            f"Unknown metric: {metric!r}. "
            f"Available: {', '.join(METRICS)}"
        )

    alg_keys = algorithms if algorithms else list(ALGORITHMS.keys())
    metric_info = METRICS[metric]

    data, labels = _load_data(csv_dir, alg_keys, benchmarks, capacitors, metric)

    if normalize:
        if normalize not in data:
            print(
                f"Error: baseline algorithm '{normalize}' has no data.",
                file=sys.stderr,
            )
            return
        data, labels = _normalize_data(data, labels, normalize)

    n_benchmarks = len(labels)
    n_algorithms = len([a for a in alg_keys if a in data])

    if n_benchmarks == 0:
        print("No data to plot.", file=sys.stderr)
        return

    _fig, ax = plt.subplots(figsize=(max(10, n_benchmarks * 1.2), 6))

    bar_width = 0.8 / n_algorithms
    x = np.arange(n_benchmarks)

    bar_idx = 0
    for alg_key in alg_keys:
        if alg_key not in data:
            continue
        alg = ALGORITHMS[alg_key]
        alg_data = data[alg_key]

        values = [alg_data.get(label, 0) for label in labels]

        offset = (bar_idx - (n_algorithms - 1) / 2) * bar_width
        bars = ax.bar(
            x + offset,
            values,
            bar_width,
            label=alg["label"],
            color=alg["color"],
            edgecolor="white",
            linewidth=0.5,
        )

        # Add value labels on bars
        for bar, val in zip(bars, values):
            if val > 0:
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    bar.get_height(),
                    f"{val:.2f}" if normalize else f"{val:g}",
                    ha="center",
                    va="bottom",
                    fontsize=7,
                    rotation=45,
                )

        bar_idx += 1

    ax.set_xlabel("Benchmark")
    if normalize:
        baseline_label = ALGORITHMS[normalize]["label"]
        ax.set_ylabel(f"Normalized to {baseline_label} (ratio)")
        ax.set_title(f"{metric_info['title']} (Normalized to {baseline_label})")
        ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.8, alpha=0.7)
    else:
        ax.set_ylabel(metric_info["ylabel"])
        ax.set_title(f"{metric_info['title']} by Algorithm")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8)
    ax.legend()
    ax.grid(axis="y", alpha=0.3)

    plt.tight_layout()

    if output_file:
        plt.savefig(str(output_file), dpi=150, bbox_inches="tight")
        print(f"Saved to {output_file}")
    else:
        plt.show()
