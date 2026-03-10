#!/usr/bin/env python3
"""Plot benchmark results comparing MILP, RockClimb, and SCHEMATIC algorithms."""

import argparse
import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


ALGORITHMS = {
    "milp": {
        "file": "../benchmarks/milp_benchmark_summary.csv",
        "label": "MILP",
        "color": "#2196F3",
    },
    "rockclimb": {
        "file": "../benchmarks/rockclimb_benchmark_summary.csv",
        "label": "RockClimb (Machine)",
        "color": "#FF9800",
    },
    "schematic": {
        "file": "../benchmarks/schematic_benchmark_summary.csv",
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


def read_csv(filepath):
    """Read a CSV file and return rows as list of dicts."""
    rows = []
    with open(filepath, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)
    return rows


def parse_benchmark_name(benchmark):
    """Split 'program-capacitor' into (program, capacitor).

    Handles multi-part program names like 'activity_recognition-100nF'.
    """
    parts = benchmark.rsplit("-", 1)
    if len(parts) == 2:
        return parts[0], parts[1]
    return benchmark, ""


def get_value(row, metric_key):
    """Extract a numeric value from a row."""
    col = METRICS[metric_key]["column"]

    if col not in row:
        return None
    val = row.get(col, "")
    if val == "" or val is None:
        return None
    try:
        return float(val)
    except ValueError:
        return None


def load_data(base_dir, algorithms, benchmarks, capacitors, metric_key):
    """Load and filter data from CSV files.

    Returns:
        data: dict[algorithm_key] -> dict[benchmark_label] -> value
        benchmark_labels: ordered list of benchmark labels to plot
    """
    data = {}
    all_labels = set()

    for alg_key in algorithms:
        alg = ALGORITHMS[alg_key]
        filepath = Path(base_dir) / alg["file"]
        if not filepath.exists():
            print(f"Warning: {filepath} not found, skipping {alg_key}", file=sys.stderr)
            continue

        rows = read_csv(filepath)
        alg_data = {}

        for row in rows:
            bm = row.get("benchmark", "").strip()
            if not bm:
                continue
            program, capacitor = parse_benchmark_name(bm)

            if benchmarks and program not in benchmarks:
                continue
            if capacitors and capacitor not in capacitors:
                continue

            label = f"{program}\n({capacitor})"
            val = get_value(row, metric_key)
            if val is not None:
                alg_data[label] = val
                all_labels.add(label)

        data[alg_key] = alg_data

    # Sort labels: group by program name, then by capacitor size
    cap_order = {"100nF": 0, "1uF": 1, "10uF": 2, "100uF": 3}

    def sort_key(label):
        parts = label.split("\n")
        program = parts[0]
        cap = parts[1].strip("()") if len(parts) > 1 else ""
        return (program, cap_order.get(cap, 99))

    benchmark_labels = sorted(all_labels, key=sort_key)
    return data, benchmark_labels


def normalize_data(data, benchmark_labels, baseline_alg):
    """Normalize all values relative to the baseline algorithm (baseline = 1.0).

    Benchmarks where the baseline has no data or zero value are dropped.
    """
    baseline_data = data.get(baseline_alg, {})
    normalized = {}
    kept_labels = []

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


def plot_grouped_bars(data, benchmark_labels, algorithms, metric_key,
                      normalize=None, output_file=None):
    """Create a grouped bar plot."""
    metric = METRICS[metric_key]

    if normalize:
        if normalize not in data:
            print(f"Error: baseline algorithm '{normalize}' has no data.", file=sys.stderr)
            return
        data, benchmark_labels = normalize_data(data, benchmark_labels, normalize)

    n_benchmarks = len(benchmark_labels)
    n_algorithms = len([a for a in algorithms if a in data])

    if n_benchmarks == 0:
        print("No data to plot.", file=sys.stderr)
        return

    fig, ax = plt.subplots(figsize=(max(10, n_benchmarks * 1.2), 6))

    bar_width = 0.8 / n_algorithms
    x = np.arange(n_benchmarks)

    bar_idx = 0
    for alg_key in algorithms:
        if alg_key not in data:
            continue
        alg = ALGORITHMS[alg_key]
        alg_data = data[alg_key]

        values = []
        for label in benchmark_labels:
            values.append(alg_data.get(label, 0))

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
        ax.set_title(f"{metric['title']} (Normalized to {baseline_label})")
        ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.8, alpha=0.7)
    else:
        ax.set_ylabel(metric["ylabel"])
        ax.set_title(f"{metric['title']} by Algorithm")
    ax.set_xticks(x)
    ax.set_xticklabels(benchmark_labels, fontsize=8)
    ax.legend()
    ax.grid(axis="y", alpha=0.3)

    plt.tight_layout()

    if output_file:
        plt.savefig(output_file, dpi=150, bbox_inches="tight")
        print(f"Saved to {output_file}")
    else:
        plt.show()


def main():
    parser = argparse.ArgumentParser(
        description="Plot benchmark results comparing checkpoint insertion algorithms.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""\
examples:
  # Plot prologue calls for all benchmarks and all algorithms
  %(prog)s --metric prologue

  # Plot execution time for specific programs
  %(prog)s --metric execution_time --benchmarks crc rsa

  # Plot only MILP and SCHEMATIC for 100nF capacitor
  %(prog)s --metric prologue --algorithms milp schematic --capacitors 100nF

  # Normalize to MILP baseline (all values shown as ratio to MILP)
  %(prog)s --metric prologue --normalize milp

  # Save to file
  %(prog)s --metric prologue -o prologue_comparison.png
""",
    )
    parser.add_argument(
        "--metric",
        choices=list(METRICS.keys()),
        default="prologue",
        help="Metric to plot (default: prologue)",
    )
    parser.add_argument(
        "--algorithms",
        nargs="+",
        choices=list(ALGORITHMS.keys()),
        default=list(ALGORITHMS.keys()),
        help="Algorithms to include (default: all)",
    )
    parser.add_argument(
        "--benchmarks",
        nargs="+",
        help="Benchmark programs to include (e.g., crc rsa chacha20). Default: all",
    )
    parser.add_argument(
        "--capacitors",
        nargs="+",
        help="Capacitor sizes to include (e.g., 100nF 1uF). Default: all",
    )
    parser.add_argument(
        "--csv-dir",
        default=str(Path(__file__).parent),
        help="Directory containing CSV files (default: same as script)",
    )
    parser.add_argument(
        "--normalize",
        choices=list(ALGORITHMS.keys()),
        help="Normalize values relative to this algorithm (baseline = 1.0)",
    )
    parser.add_argument(
        "-o", "--output",
        help="Output file path (e.g., plot.png). If omitted, shows interactive window",
    )

    args = parser.parse_args()

    data, labels = load_data(
        args.csv_dir,
        args.algorithms,
        args.benchmarks,
        args.capacitors,
        args.metric,
    )

    plot_grouped_bars(data, labels, args.algorithms, args.metric,
                      normalize=args.normalize, output_file=args.output)


if __name__ == "__main__":
    main()
