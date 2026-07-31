"""Compare baseline MILP results against coarse-allocation results."""

from __future__ import annotations

import csv
import math
import statistics
from pathlib import Path
from typing import Any

from ..errors import ConfigError

_SUMMARY_FIELDS = [
    "capacitor",
    "benchmark_count",
    "solve_time_mean_before_ms",
    "solve_time_mean_after_ms",
    "solve_time_mean_reduction_pct",
    "solve_time_geomean_reduction_pct",
    "solve_time_mean_of_reductions_pct",
    "solve_time_median_before_ms",
    "solve_time_median_after_ms",
    "solve_time_median_reduction_pct",
    "worst_case_before_benchmark",
    "worst_case_before_ms",
    "worst_case_after_benchmark",
    "worst_case_after_ms",
    "milp_variables_mean_before",
    "milp_variables_mean_after",
    "milp_variables_mean_reduction_pct",
    "milp_constraints_mean_before",
    "milp_constraints_mean_after",
    "milp_constraints_mean_reduction_pct",
    "milp_presolved_variables_mean_before",
    "milp_presolved_variables_mean_after",
    "milp_presolved_variables_mean_reduction_pct",
    "milp_presolved_constraints_mean_before",
    "milp_presolved_constraints_mean_after",
    "milp_presolved_constraints_mean_reduction_pct",
]


def _cap_sort_key(capacitor: str) -> tuple[float, str]:
    if capacitor.endswith("uF"):
        try:
            return float(capacitor.removesuffix("uF")), capacitor
        except ValueError:
            pass
    return float("inf"), capacitor


def _mean(values: list[float]) -> float:
    if not values:
        raise ConfigError("Cannot summarize an empty result set.")
    return sum(values) / len(values)


def _percent_reduction(before: float, after: float) -> float:
    if before <= 0:
        raise ConfigError(
            f"Expected a positive baseline value for reduction, got {before}."
        )
    return (before - after) / before * 100.0


def _geometric_mean(values: list[float]) -> float:
    if not values:
        raise ConfigError("Cannot compute a geometric mean over an empty result set.")
    if any(value <= 0 for value in values):
        raise ConfigError("Geometric mean requires strictly positive values.")
    return math.exp(sum(math.log(value) for value in values) / len(values))


def _read_rows(csv_path: Path) -> dict[str, dict[str, str]]:
    with csv_path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))

    if not rows:
        raise ConfigError(f"No data rows found in {csv_path}.")

    by_benchmark: dict[str, dict[str, str]] = {}
    for row in rows:
        benchmark = row.get("benchmark", "").strip()
        if not benchmark:
            raise ConfigError(f"Missing benchmark column in {csv_path}.")
        if benchmark in by_benchmark:
            raise ConfigError(f"Duplicate benchmark '{benchmark}' in {csv_path}.")
        by_benchmark[benchmark] = row

    return by_benchmark


def _parse_float(
    row: dict[str, str],
    column: str,
    csv_path: Path,
    benchmark: str,
) -> float:
    raw_value = row.get(column, "").strip()
    if not raw_value:
        raise ConfigError(
            f"Missing column '{column}' for benchmark '{benchmark}' in {csv_path}."
        )
    try:
        return float(raw_value)
    except ValueError as exc:
        raise ConfigError(
            f"Invalid numeric value for '{column}' in benchmark '{benchmark}' "
            f"from {csv_path}: {raw_value}"
        ) from exc


def _build_pairs(
    baseline_rows: dict[str, dict[str, str]],
    baseline_csv: Path,
    coarse_rows: dict[str, dict[str, str]],
    coarse_csv: Path,
) -> list[dict[str, Any]]:
    baseline_benchmarks = set(baseline_rows)
    coarse_benchmarks = set(coarse_rows)
    if baseline_benchmarks != coarse_benchmarks:
        missing_from_coarse = sorted(baseline_benchmarks - coarse_benchmarks)
        missing_from_baseline = sorted(coarse_benchmarks - baseline_benchmarks)
        details: list[str] = []
        if missing_from_coarse:
            details.append(f"missing from coarse CSV: {', '.join(missing_from_coarse)}")
        if missing_from_baseline:
            details.append(
                f"missing from baseline CSV: {', '.join(missing_from_baseline)}"
            )
        raise ConfigError("CSV benchmark sets do not match; " + "; ".join(details))

    pairs: list[dict[str, Any]] = []
    for benchmark in sorted(baseline_rows):
        baseline = baseline_rows[benchmark]
        coarse = coarse_rows[benchmark]

        baseline_capacitor = baseline.get("capacitor", "").strip()
        coarse_capacitor = coarse.get("capacitor", "").strip()
        if baseline_capacitor != coarse_capacitor:
            raise ConfigError(
                f"Capacitor mismatch for benchmark '{benchmark}': "
                f"{baseline_capacitor} vs {coarse_capacitor}"
            )

        pairs.append(
            {
                "benchmark": benchmark,
                "capacitor": baseline_capacitor,
                "solve_time_before_ms": _parse_float(
                    baseline,
                    "milp_solve_time_ms",
                    baseline_csv,
                    benchmark,
                ),
                "solve_time_after_ms": _parse_float(
                    coarse,
                    "milp_solve_time_ms",
                    coarse_csv,
                    benchmark,
                ),
                "variables_before": _parse_float(
                    baseline,
                    "milp_variables",
                    baseline_csv,
                    benchmark,
                ),
                "variables_after": _parse_float(
                    coarse,
                    "milp_variables",
                    coarse_csv,
                    benchmark,
                ),
                "constraints_before": _parse_float(
                    baseline,
                    "milp_constraints",
                    baseline_csv,
                    benchmark,
                ),
                "constraints_after": _parse_float(
                    coarse,
                    "milp_constraints",
                    coarse_csv,
                    benchmark,
                ),
                "presolved_variables_before": _parse_float(
                    baseline,
                    "milp_presolved_variables",
                    baseline_csv,
                    benchmark,
                ),
                "presolved_variables_after": _parse_float(
                    coarse,
                    "milp_presolved_variables",
                    coarse_csv,
                    benchmark,
                ),
                "presolved_constraints_before": _parse_float(
                    baseline,
                    "milp_presolved_constraints",
                    baseline_csv,
                    benchmark,
                ),
                "presolved_constraints_after": _parse_float(
                    coarse,
                    "milp_presolved_constraints",
                    coarse_csv,
                    benchmark,
                ),
            }
        )

    return pairs


def _summarize_pairs(
    pairs: list[dict[str, Any]],
    capacitor: str,
) -> dict[str, str | int | float]:
    solve_before = [float(pair["solve_time_before_ms"]) for pair in pairs]
    solve_after = [float(pair["solve_time_after_ms"]) for pair in pairs]
    solve_reductions = [
        _percent_reduction(before, after)
        for before, after in zip(solve_before, solve_after)
    ]
    solve_ratios = [after / before for before, after in zip(solve_before, solve_after)]

    worst_case_before = max(pairs, key=lambda pair: float(pair["solve_time_before_ms"]))
    worst_case_after = max(pairs, key=lambda pair: float(pair["solve_time_after_ms"]))

    variables_before = [float(pair["variables_before"]) for pair in pairs]
    variables_after = [float(pair["variables_after"]) for pair in pairs]
    constraints_before = [float(pair["constraints_before"]) for pair in pairs]
    constraints_after = [float(pair["constraints_after"]) for pair in pairs]
    presolved_variables_before = [
        float(pair["presolved_variables_before"]) for pair in pairs
    ]
    presolved_variables_after = [
        float(pair["presolved_variables_after"]) for pair in pairs
    ]
    presolved_constraints_before = [
        float(pair["presolved_constraints_before"]) for pair in pairs
    ]
    presolved_constraints_after = [
        float(pair["presolved_constraints_after"]) for pair in pairs
    ]

    mean_solve_before = _mean(solve_before)
    mean_solve_after = _mean(solve_after)
    median_solve_before = statistics.median(solve_before)
    median_solve_after = statistics.median(solve_after)

    return {
        "capacitor": capacitor,
        "benchmark_count": len(pairs),
        "solve_time_mean_before_ms": mean_solve_before,
        "solve_time_mean_after_ms": mean_solve_after,
        "solve_time_mean_reduction_pct": _percent_reduction(
            mean_solve_before,
            mean_solve_after,
        ),
        "solve_time_geomean_reduction_pct": _percent_reduction(
            1.0,
            _geometric_mean(solve_ratios),
        ),
        "solve_time_mean_of_reductions_pct": _mean(solve_reductions),
        "solve_time_median_before_ms": median_solve_before,
        "solve_time_median_after_ms": median_solve_after,
        "solve_time_median_reduction_pct": _percent_reduction(
            median_solve_before,
            median_solve_after,
        ),
        "worst_case_before_benchmark": str(worst_case_before["benchmark"]),
        "worst_case_before_ms": float(worst_case_before["solve_time_before_ms"]),
        "worst_case_after_benchmark": str(worst_case_after["benchmark"]),
        "worst_case_after_ms": float(worst_case_after["solve_time_after_ms"]),
        "milp_variables_mean_before": _mean(variables_before),
        "milp_variables_mean_after": _mean(variables_after),
        "milp_variables_mean_reduction_pct": _percent_reduction(
            _mean(variables_before),
            _mean(variables_after),
        ),
        "milp_constraints_mean_before": _mean(constraints_before),
        "milp_constraints_mean_after": _mean(constraints_after),
        "milp_constraints_mean_reduction_pct": _percent_reduction(
            _mean(constraints_before),
            _mean(constraints_after),
        ),
        "milp_presolved_variables_mean_before": _mean(presolved_variables_before),
        "milp_presolved_variables_mean_after": _mean(presolved_variables_after),
        "milp_presolved_variables_mean_reduction_pct": _percent_reduction(
            _mean(presolved_variables_before),
            _mean(presolved_variables_after),
        ),
        "milp_presolved_constraints_mean_before": _mean(presolved_constraints_before),
        "milp_presolved_constraints_mean_after": _mean(presolved_constraints_after),
        "milp_presolved_constraints_mean_reduction_pct": _percent_reduction(
            _mean(presolved_constraints_before),
            _mean(presolved_constraints_after),
        ),
    }


def summarize_milp_coarse_allocation(
    baseline_csv: Path,
    coarse_csv: Path,
) -> list[dict[str, str | int | float]]:
    baseline_rows = _read_rows(baseline_csv)
    coarse_rows = _read_rows(coarse_csv)
    pairs = _build_pairs(baseline_rows, baseline_csv, coarse_rows, coarse_csv)

    capacitors = sorted(
        {str(pair["capacitor"]) for pair in pairs},
        key=_cap_sort_key,
    )

    summaries = [
        _summarize_pairs(
            [pair for pair in pairs if str(pair["capacitor"]) == capacitor],
            capacitor,
        )
        for capacitor in capacitors
    ]
    summaries.append(_summarize_pairs(pairs, "ALL"))
    return summaries


def write_milp_coarse_summary_csv(
    summary_rows: list[dict[str, str | int | float]],
    output_path: Path,
) -> None:
    with output_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=_SUMMARY_FIELDS)
        writer.writeheader()
        writer.writerows(summary_rows)


def format_milp_coarse_summary(
    summary_rows: list[dict[str, str | int | float]],
) -> str:
    lines: list[str] = []
    for row in summary_rows:
        capacitor = str(row["capacitor"])
        lines.append(
            (
                f"{capacitor}: solve mean "
                f"{float(row['solve_time_mean_before_ms']):.2f} -> "
                f"{float(row['solve_time_mean_after_ms']):.2f} ms "
                f"({float(row['solve_time_mean_reduction_pct']):.1f}% reduction), "
                f"geomean reduction "
                f"{float(row['solve_time_geomean_reduction_pct']):.1f}%, "
                f"mean per-benchmark reduction "
                f"{float(row['solve_time_mean_of_reductions_pct']):.1f}%."
            )
        )
        lines.append(
            (
                f"{capacitor}: worst case "
                f"{str(row['worst_case_before_benchmark'])} "
                f"{float(row['worst_case_before_ms']):.0f} ms -> "
                f"{str(row['worst_case_after_benchmark'])} "
                f"{float(row['worst_case_after_ms']):.0f} ms."
            )
        )
        lines.append(
            (
                f"{capacitor}: mean MILP vars "
                f"{float(row['milp_variables_mean_before']):.1f} -> "
                f"{float(row['milp_variables_mean_after']):.1f} "
                f"({float(row['milp_variables_mean_reduction_pct']):.1f}%), "
                f"constraints {float(row['milp_constraints_mean_before']):.1f} -> "
                f"{float(row['milp_constraints_mean_after']):.1f} "
                f"({float(row['milp_constraints_mean_reduction_pct']):.1f}%)."
            )
        )
        lines.append(
            (
                f"{capacitor}: mean presolved vars "
                f"{float(row['milp_presolved_variables_mean_before']):.1f} -> "
                f"{float(row['milp_presolved_variables_mean_after']):.1f} "
                f"({float(row['milp_presolved_variables_mean_reduction_pct']):.1f}%), "
                f"presolved constraints "
                f"{float(row['milp_presolved_constraints_mean_before']):.1f} -> "
                f"{float(row['milp_presolved_constraints_mean_after']):.1f} "
                f"({float(row['milp_presolved_constraints_mean_reduction_pct']):.1f}%)."
            )
        )

    return "\n".join(lines)
