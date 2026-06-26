"""Unit tests for MILP benchmark CSV wiring."""

from __future__ import annotations

from ckpt.bench.milp import CSV_HEADER, build_row
from ckpt.output_parser import PassStatistics


def test_milp_csv_includes_problem_size_columns() -> None:
    assert "abstract_cfg_blocks" in CSV_HEADER
    assert "abstract_cfg_edges" in CSV_HEADER
    assert "milp_allocation_mode" in CSV_HEADER
    assert "milp_variables" in CSV_HEADER
    assert "milp_constraints" in CSV_HEADER
    assert "milp_presolved_variables" in CSV_HEADER
    assert "milp_presolved_constraints" in CSV_HEADER


def test_milp_row_includes_presolved_problem_size() -> None:
    stats = PassStatistics(
        milp_allocation_mode="coarse",
        milp_variables=100,
        milp_constraints=200,
        milp_presolved_variables=60,
        milp_presolved_constraints=120,
        optimal_solution="yes",
    )
    row = build_row("crc", "1uF", stats, None, "")
    assert row["milp_allocation_mode"] == "coarse"
    assert row["milp_variables"] == 100
    assert row["milp_constraints"] == 200
    assert row["milp_presolved_variables"] == 60
    assert row["milp_presolved_constraints"] == 120
