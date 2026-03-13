"""Unit tests for bench runner helpers and StepResult — pure functions."""

from __future__ import annotations

import pytest

from ckpt.bench.runner import build_base_fields, nvm_counter
from ckpt.output_parser import NvmCounters, PassStatistics
from ckpt.runner import StepResult


# ---------------------------------------------------------------------------
# StepResult.output
# ---------------------------------------------------------------------------

class TestStepResultOutput:
    def test_both_populated(self):
        r = StepResult(returncode=0, stdout="out\n", stderr="err\n", duration_ms=10)
        assert r.output == "out\nerr\n"

    def test_one_empty(self):
        r = StepResult(returncode=0, stdout="out", stderr="", duration_ms=0)
        assert r.output == "out"

    def test_both_empty(self):
        r = StepResult(returncode=0, stdout="", stderr="", duration_ms=0)
        assert r.output == ""


# ---------------------------------------------------------------------------
# nvm_counter
# ---------------------------------------------------------------------------

class TestNvmCounter:
    def test_none_nvm(self):
        assert nvm_counter(None, "region_boundary") == 0

    def test_none_attr_value(self):
        c = NvmCounters(region_boundary=None)
        assert nvm_counter(c, "region_boundary") == 0

    def test_valid_value(self):
        c = NvmCounters(region_boundary=5)
        assert nvm_counter(c, "region_boundary") == 5

    def test_missing_attr(self):
        c = NvmCounters()
        assert nvm_counter(c, "nonexistent") == 0


# ---------------------------------------------------------------------------
# build_base_fields
# ---------------------------------------------------------------------------

class TestBuildBaseFields:
    def test_full_stats(self):
        stats = PassStatistics(
            basic_blocks=24,
            edges=30,
            regions=3,
            compilation_time_ms=500,
            peak_rss_kb=10240,
        )
        output = "some output\nRESULT: 42\nmore"
        fields = build_base_fields(stats, output)
        assert fields["basic_blocks"] == 24
        assert fields["edges"] == 30
        assert fields["regions"] == 3
        assert fields["compilation_time_ms"] == 500
        assert fields["peak_rss_kb"] == 10240
        assert fields["result"] == "42"

    def test_all_none_stats(self):
        stats = PassStatistics()
        fields = build_base_fields(stats, "no result line")
        assert fields["basic_blocks"] == 0
        assert fields["edges"] == 0
        assert fields["regions"] == 0
        assert fields["compilation_time_ms"] == 0
        assert fields["peak_rss_kb"] == 0
        assert fields["result"] == ""
