"""Unit tests for ckpt.analysis.plot helpers — pure functions."""

from __future__ import annotations

import pytest

from ckpt.analysis.plot import _get_value, _parse_benchmark_name


# ---------------------------------------------------------------------------
# _parse_benchmark_name
# ---------------------------------------------------------------------------

class TestParseBenchmarkName:
    @pytest.mark.parametrize(
        "bench, expected",
        [
            ("crc-1uF", ("crc", "1uF")),
            ("nodash", ("nodash", "")),
            ("activity_recognition-100uF", ("activity_recognition", "100uF")),
            ("a-b-c", ("a-b", "c")),
        ],
        ids=["simple", "no-dash", "underscore-with-dash", "multi-dash"],
    )
    def test_parse(self, bench, expected):
        assert _parse_benchmark_name(bench) == expected


# ---------------------------------------------------------------------------
# _get_value
# ---------------------------------------------------------------------------

class TestGetValue:
    def test_valid_float(self):
        row = {"runtime_region_prologue_calls": "42.5"}
        assert _get_value(row, "prologue") == 42.5

    def test_integer_string(self):
        row = {"compilation_time_ms": "500"}
        assert _get_value(row, "compilation_time") == 500.0

    def test_empty_string(self):
        row = {"runtime_region_prologue_calls": ""}
        assert _get_value(row, "prologue") is None

    def test_non_numeric(self):
        row = {"runtime_region_prologue_calls": "N/A"}
        assert _get_value(row, "prologue") is None

    def test_missing_column(self):
        row = {}
        assert _get_value(row, "prologue") is None
