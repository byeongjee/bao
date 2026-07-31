"""Unit tests for MILP coarse-allocation result analysis."""

from __future__ import annotations

import csv
from pathlib import Path

import pytest
from ckpt.analysis.milp_coarse import summarize_milp_coarse_allocation
from ckpt.errors import ConfigError

pytestmark = pytest.mark.unit


_FIELDS = [
    "benchmark",
    "capacitor",
    "milp_solve_time_ms",
    "milp_variables",
    "milp_constraints",
    "milp_presolved_variables",
    "milp_presolved_constraints",
]


def _write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=_FIELDS)
        writer.writeheader()
        writer.writerows(rows)


def test_summarize_milp_coarse_allocation_reports_per_cap_and_overall(
    tmp_path: Path,
) -> None:
    baseline_csv = tmp_path / "baseline.csv"
    coarse_csv = tmp_path / "coarse.csv"

    _write_csv(
        baseline_csv,
        [
            {
                "benchmark": "alpha-5uF",
                "capacitor": "5uF",
                "milp_solve_time_ms": "100",
                "milp_variables": "10",
                "milp_constraints": "20",
                "milp_presolved_variables": "8",
                "milp_presolved_constraints": "16",
            },
            {
                "benchmark": "beta-5uF",
                "capacitor": "5uF",
                "milp_solve_time_ms": "25",
                "milp_variables": "20",
                "milp_constraints": "40",
                "milp_presolved_variables": "12",
                "milp_presolved_constraints": "24",
            },
            {
                "benchmark": "gamma-10uF",
                "capacitor": "10uF",
                "milp_solve_time_ms": "50",
                "milp_variables": "30",
                "milp_constraints": "60",
                "milp_presolved_variables": "20",
                "milp_presolved_constraints": "30",
            },
        ],
    )
    _write_csv(
        coarse_csv,
        [
            {
                "benchmark": "alpha-5uF",
                "capacitor": "5uF",
                "milp_solve_time_ms": "50",
                "milp_variables": "8",
                "milp_constraints": "18",
                "milp_presolved_variables": "6",
                "milp_presolved_constraints": "12",
            },
            {
                "benchmark": "beta-5uF",
                "capacitor": "5uF",
                "milp_solve_time_ms": "20",
                "milp_variables": "18",
                "milp_constraints": "38",
                "milp_presolved_variables": "10",
                "milp_presolved_constraints": "20",
            },
            {
                "benchmark": "gamma-10uF",
                "capacitor": "10uF",
                "milp_solve_time_ms": "25",
                "milp_variables": "24",
                "milp_constraints": "54",
                "milp_presolved_variables": "14",
                "milp_presolved_constraints": "24",
            },
        ],
    )

    summaries = summarize_milp_coarse_allocation(baseline_csv, coarse_csv)

    assert [row["capacitor"] for row in summaries] == ["5uF", "10uF", "ALL"]

    five_uf = summaries[0]
    assert five_uf["benchmark_count"] == 2
    assert five_uf["solve_time_mean_before_ms"] == pytest.approx(62.5)
    assert five_uf["solve_time_mean_after_ms"] == pytest.approx(35.0)
    assert five_uf["solve_time_mean_reduction_pct"] == pytest.approx(44.0)
    assert five_uf["solve_time_geomean_reduction_pct"] == pytest.approx(36.7544468)
    assert five_uf["solve_time_mean_of_reductions_pct"] == pytest.approx(35.0)
    assert five_uf["worst_case_before_benchmark"] == "alpha-5uF"
    assert five_uf["worst_case_before_ms"] == pytest.approx(100.0)
    assert five_uf["worst_case_after_benchmark"] == "alpha-5uF"
    assert five_uf["worst_case_after_ms"] == pytest.approx(50.0)
    assert five_uf["milp_variables_mean_reduction_pct"] == pytest.approx(13.3333333)
    assert five_uf["milp_presolved_constraints_mean_reduction_pct"] == pytest.approx(
        20.0
    )

    overall = summaries[2]
    assert overall["benchmark_count"] == 3
    assert overall["solve_time_mean_before_ms"] == pytest.approx(58.3333333)
    assert overall["solve_time_mean_after_ms"] == pytest.approx(31.6666667)
    assert overall["solve_time_mean_reduction_pct"] == pytest.approx(45.7142857)
    assert overall["solve_time_geomean_reduction_pct"] == pytest.approx(41.5196452)


def test_summarize_milp_coarse_allocation_rejects_mismatched_benchmarks(
    tmp_path: Path,
) -> None:
    baseline_csv = tmp_path / "baseline.csv"
    coarse_csv = tmp_path / "coarse.csv"

    _write_csv(
        baseline_csv,
        [
            {
                "benchmark": "alpha-5uF",
                "capacitor": "5uF",
                "milp_solve_time_ms": "100",
                "milp_variables": "10",
                "milp_constraints": "20",
                "milp_presolved_variables": "8",
                "milp_presolved_constraints": "16",
            }
        ],
    )
    _write_csv(
        coarse_csv,
        [
            {
                "benchmark": "beta-5uF",
                "capacitor": "5uF",
                "milp_solve_time_ms": "50",
                "milp_variables": "8",
                "milp_constraints": "18",
                "milp_presolved_variables": "6",
                "milp_presolved_constraints": "12",
            }
        ],
    )

    with pytest.raises(ConfigError, match="CSV benchmark sets do not match"):
        summarize_milp_coarse_allocation(baseline_csv, coarse_csv)
