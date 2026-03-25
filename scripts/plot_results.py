#!/usr/bin/env python3
"""Visualize benchmark results from result/ directory.

Reads *-swbor.csv for runtime region boundary metrics and
*-swbor-no-debug.csv for execution/profiling/compilation times.

Usage:
    uv run --extra plot python scripts/plot_results.py [OPTIONS]

Options:
    --result-dir DIR    Result directory (default: result/)
    --normalize         Normalize values (w.r.t. uninstrumented if available, else milp)
    --output-dir DIR    Save plots to directory instead of showing
    --benchmarks B...   Filter to specific benchmarks (default: all)
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


# -- Algorithm definitions --------------------------------------------------

ALGORITHMS = ["milp", "schematic", "rockclimb", "schematicO3"]

ALG_STYLE = {
    "milp":          {"label": "MILP",          "color": "#2196F3"},
    "schematic":     {"label": "SCHEMATIC",     "color": "#4CAF50"},
    "rockclimb":     {"label": "RockClimb",     "color": "#FF9800"},
    "schematicO3":   {"label": "SCHEMATIC-O3",  "color": "#8BC34A"},
    "uninstrumented": {"label": "Uninstrumented", "color": "#9E9E9E"},
}

# -- Metric definitions -----------------------------------------------------
# Each metric specifies:
#   source: "swbor" (device-debug CSVs) or "no-debug" (no-device-debug CSVs)
#   column: CSV column name
#   include_uninstrumented: whether to show uninstrumented bars
#   ylabel: y-axis label

METRICS = {
    "region_boundaries": {
        "source": "swbor",
        "column": "region_boundaries",
        "include_uninstrumented": False,
        "ylabel": "# Region Boundaries (static)",
        "title": "Region Boundaries",
    },
    "runtime_region_boundary_calls": {
        "source": "swbor",
        "column": "runtime_region_boundary_calls",
        "include_uninstrumented": False,
        "ylabel": "# Runtime Region Boundary Calls",
        "title": "Runtime Region Boundary Calls",
    },
    "execution_time": {
        "source": "no-debug",
        "column": "execution_time_us",
        "include_uninstrumented": True,
        "ylabel": "Execution Time (us)",
        "title": "Execution Time",
    },
    "profiling_time": {
        "source": "no-debug",
        "column": "profiling_time_ms",
        "include_uninstrumented": False,
        "ylabel": "Profiling Time (ms)",
        "title": "Profiling Time",
    },
    "compilation_time": {
        "source": "no-debug",
        "column": "compilation_time_ms",
        "include_uninstrumented": True,
        "ylabel": "Compilation Time (ms)",
        "title": "Compilation Time",
    },
}


# -- Data loading -----------------------------------------------------------

def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def parse_benchmark_cap(benchmark_field: str) -> tuple[str, str]:
    """Split 'name-cap' into (name, cap). E.g. 'aes-1uF' -> ('aes', '1uF')."""
    parts = benchmark_field.rsplit("-", 1)
    if len(parts) == 2:
        return parts[0], parts[1]
    return benchmark_field, ""


def resolve_algorithm_csv_path(result_dir: Path, algo: str, source: str) -> Path | None:
    """Resolve current or legacy algorithm CSV naming."""
    candidates = {
        "swbor": [
            result_dir / f"{algo}_debug.csv",
            result_dir / f"{algo}-swbor.csv",
        ],
        "no-debug": [
            result_dir / f"{algo}.csv",
            result_dir / f"{algo}-swbor-no-debug.csv",
        ],
    }

    for path in candidates[source]:
        if path.exists():
            return path
    return None


def discover_capacitors(result_dir: Path) -> list[str]:
    """Discover capacitor labels across all available algorithm CSVs."""
    capacitors: set[str] = set()
    for algo in ALGORITHMS:
        for source in ["swbor", "no-debug"]:
            path = resolve_algorithm_csv_path(result_dir, algo, source)
            if path is None:
                continue
            for row in read_csv(path):
                bm = row.get("benchmark", "").strip()
                if not bm:
                    continue
                _name, cap = parse_benchmark_cap(bm)
                if cap:
                    capacitors.add(cap)

    def capacitor_sort_key(cap: str) -> tuple[float, str]:
        try:
            return float(cap.removesuffix("uF")), cap
        except ValueError:
            return float("inf"), cap

    return sorted(capacitors, key=capacitor_sort_key)


def load_algorithm_data(
    result_dir: Path,
    algo: str,
    source: str,
    column: str,
) -> dict[tuple[str, str], float]:
    """Load {(benchmark, cap): value} for an algorithm.

    source: 'swbor' or 'no-debug'.
    """
    path = resolve_algorithm_csv_path(result_dir, algo, source)
    if path is None:
        return {}

    data: dict[tuple[str, str], float] = {}
    for row in read_csv(path):
        bm = row.get("benchmark", "").strip()
        if not bm:
            continue
        name, cap = parse_benchmark_cap(bm)
        val_str = row.get(column, "")
        if val_str:
            try:
                data[(name, cap)] = float(val_str)
            except ValueError:
                pass
    return data


def load_uninstrumented_data(
    result_dir: Path,
    column: str,
) -> dict[str, float]:
    """Load {benchmark: value} for uninstrumented (no capacitor dimension)."""
    path = result_dir / "uninstrumented.csv"
    if not path.exists():
        return {}

    data: dict[str, float] = {}
    for row in read_csv(path):
        bm = row.get("benchmark", "").strip()
        if not bm:
            continue
        val_str = row.get(column, "")
        if val_str:
            try:
                data[bm] = float(val_str)
            except ValueError:
                pass
    return data


# -- Plotting ---------------------------------------------------------------

def plot_metric_for_cap(
    cap: str,
    metric_key: str,
    metric_info: dict,
    algo_data: dict[str, dict[tuple[str, str], float]],
    uninst_data: dict[str, float],
    benchmarks: list[str],
    normalize: bool,
) -> plt.Figure:
    """Create a bar chart for one (metric, capacitor) combination."""
    del metric_key
    include_uninst = metric_info["include_uninstrumented"] and uninst_data

    # Determine which algorithms have data for this cap
    active_algos: list[str] = []
    for algo in ALGORITHMS:
        if any((bm, cap) in algo_data.get(algo, {}) for bm in benchmarks):
            active_algos.append(algo)
    if include_uninst:
        active_algos.append("uninstrumented")

    if not active_algos:
        return None

    n_benchmarks = len(benchmarks)
    n_algos = len(active_algos)

    # Determine normalization baseline
    norm_algo = None
    if normalize:
        if include_uninst:
            norm_algo = "uninstrumented"
        else:
            norm_algo = "milp"

    # Build value matrix: algo -> [val_per_benchmark]
    values: dict[str, list[float | None]] = {}
    for algo in active_algos:
        vals: list[float | None] = []
        for bm in benchmarks:
            if algo == "uninstrumented":
                v = uninst_data.get(bm)
            else:
                v = algo_data.get(algo, {}).get((bm, cap))
            vals.append(v)
        values[algo] = vals

    # Normalize
    if normalize and norm_algo and norm_algo in values:
        base_vals = values[norm_algo]
        for algo in active_algos:
            values[algo] = [
                (v / b if v is not None and b is not None and b != 0 else None)
                for v, b in zip(values[algo], base_vals)
            ]

    fig, ax = plt.subplots(figsize=(max(8, n_benchmarks * 1.5 + 1), 5))

    bar_width = 0.8 / n_algos
    x = np.arange(n_benchmarks)
    positive_values = [
        val
        for algo_values in values.values()
        for val in algo_values
        if val is not None and val > 0
    ]
    use_symlog = False
    if len(positive_values) >= 2:
        use_symlog = max(positive_values) / min(positive_values) >= 100

    for i, algo in enumerate(active_algos):
        style = ALG_STYLE[algo]
        offset = (i - (n_algos - 1) / 2) * bar_width
        plot_vals = [v if v is not None else 0 for v in values[algo]]

        bars = ax.bar(
            x + offset,
            plot_vals,
            bar_width,
            label=style["label"],
            color=style["color"],
            edgecolor="white",
            linewidth=0.5,
        )

        for bar, val in zip(bars, values[algo]):
            if val is not None and val > 0:
                label_text = f"{val:.2f}" if normalize else f"{val:g}"
                ax.text(
                    bar.get_x() + bar.get_width() / 2,
                    bar.get_height(),
                    label_text,
                    ha="center",
                    va="bottom",
                    fontsize=7,
                    rotation=45,
                )

    ax.set_xlabel("Benchmark")
    ax.set_xticks(x)
    ax.set_xticklabels(benchmarks, fontsize=9)

    if normalize and norm_algo:
        norm_label = ALG_STYLE[norm_algo]["label"]
        ax.set_ylabel(f"Normalized to {norm_label}")
        title = f"{metric_info['title']} — {cap} (normalized to {norm_label})"
        ax.axhline(y=1.0, color="gray", linestyle="--", linewidth=0.8, alpha=0.7)
    else:
        ax.set_ylabel(metric_info["ylabel"])
        title = f"{metric_info['title']} — {cap}"

    if use_symlog:
        ax.set_yscale("symlog", linthresh=1.0)
        title = f"{title} [symlog]"

    ax.set_title(title)

    ax.legend(fontsize=8)
    ax.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    return fig


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot benchmark results.")
    parser.add_argument(
        "--result-dir", type=Path, default=Path("result"),
        help="Directory containing CSV result files (default: result/)",
    )
    parser.add_argument(
        "--normalize", action="store_true",
        help="Normalize values (w.r.t. uninstrumented if available, else milp)",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=None,
        help="Save plots to this directory instead of displaying",
    )
    parser.add_argument(
        "--benchmarks", nargs="*", default=None,
        help="Filter to specific benchmark names (default: all found)",
    )
    parser.add_argument(
        "--metrics", nargs="*", default=None,
        choices=list(METRICS.keys()),
        help="Metrics to plot (default: all)",
    )
    args = parser.parse_args()

    result_dir: Path = args.result_dir
    if not result_dir.is_dir():
        print(f"Error: {result_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)

    metrics_to_plot = args.metrics if args.metrics else list(METRICS.keys())

    # Discover benchmarks from all available CSVs
    all_benchmarks: set[str] = set()
    for algo in ALGORITHMS:
        for source in ["swbor", "no-debug"]:
            path = resolve_algorithm_csv_path(result_dir, algo, source)
            if path is None:
                continue
            for row in read_csv(path):
                bm = row.get("benchmark", "").strip()
                if bm:
                    name, _ = parse_benchmark_cap(bm)
                    all_benchmarks.add(name)

    uninst_path = result_dir / "uninstrumented.csv"
    if uninst_path.exists():
        for row in read_csv(uninst_path):
            bm = row.get("benchmark", "").strip()
            if bm:
                all_benchmarks.add(bm)

    if args.benchmarks:
        benchmarks = [b for b in args.benchmarks if b in all_benchmarks]
    else:
        # Exclude 'test' by default if there are other benchmarks
        real_benchmarks = all_benchmarks - {"test"}
        benchmarks = sorted(real_benchmarks) if real_benchmarks else sorted(all_benchmarks)

    if not benchmarks:
        print("No benchmarks found.", file=sys.stderr)
        sys.exit(1)

    print(f"Benchmarks: {benchmarks}")
    print(f"Metrics: {metrics_to_plot}")
    capacitors = discover_capacitors(result_dir)
    print(f"Capacitors: {capacitors}")

    for metric_key in metrics_to_plot:
        metric_info = METRICS[metric_key]
        source = metric_info["source"]
        column = metric_info["column"]

        # Load data for all algorithms
        algo_data: dict[str, dict[tuple[str, str], float]] = {}
        for algo in ALGORITHMS:
            algo_data[algo] = load_algorithm_data(result_dir, algo, source, column)

        uninst_data: dict[str, float] = {}
        if metric_info["include_uninstrumented"]:
            uninst_data = load_uninstrumented_data(result_dir, column)

        for cap in capacitors:
            fig = plot_metric_for_cap(
                cap, metric_key, metric_info,
                algo_data, uninst_data, benchmarks, args.normalize,
            )
            if fig is None:
                continue

            if args.output_dir:
                norm_suffix = "_normalized" if args.normalize else ""
                filename = f"{metric_key}_{cap}{norm_suffix}.png"
                fig.savefig(
                    str(args.output_dir / filename),
                    dpi=150, bbox_inches="tight",
                )
                print(f"Saved {args.output_dir / filename}")
                plt.close(fig)

    if not args.output_dir:
        plt.show()


if __name__ == "__main__":
    main()
