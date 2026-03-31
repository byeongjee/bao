"""Unit tests for MILP benchmark CSV wiring."""

from __future__ import annotations

from ckpt.bench.milp import _CSV_HEADER


def test_milp_csv_includes_abstract_cfg_size() -> None:
    assert "abstract_cfg_size" in _CSV_HEADER
