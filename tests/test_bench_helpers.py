"""Unit tests for bench runner helpers and StepResult — pure functions."""

from __future__ import annotations

from pathlib import Path

import pytest
from ckpt.bench.runner import CompileResult, nvm_counter
from ckpt.output_parser import NvmCounters
from ckpt.runner import StepResult

pytestmark = pytest.mark.unit


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

    def test_large_value(self):
        c = NvmCounters(region_boundary=70000)
        assert nvm_counter(c, "region_boundary") == 70000

    def test_missing_attr(self):
        c = NvmCounters()
        assert nvm_counter(c, "nonexistent") == 0


# ---------------------------------------------------------------------------
# CompileResult
# ---------------------------------------------------------------------------


class TestCompileResult:
    def test_fields(self):
        cr = CompileResult(
            out_dir=Path("/tmp/out"),
            pass_output="some output",
            stats_json=None,
            profiling_time_ms=123,
        )
        assert cr.out_dir == Path("/tmp/out")
        assert cr.pass_output == "some output"
        assert cr.stats_json is None
        assert cr.profiling_time_ms == 123

    def test_with_stats_json(self):
        p = Path("/tmp/stats.json")
        cr = CompileResult(
            out_dir=Path("/tmp/out"),
            pass_output="",
            stats_json=p,
            profiling_time_ms=0,
        )
        assert cr.stats_json == p
