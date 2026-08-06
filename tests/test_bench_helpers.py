"""Unit tests for bench runner helpers and StepResult — pure functions."""

from __future__ import annotations

from pathlib import Path

import pytest
from ckpt.bench.all import (
    ALL_ALGORITHMS,
    DEFAULT_ALGORITHMS,
    StepOutcome,
    all_ok,
    plan_steps,
)
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


# ---------------------------------------------------------------------------
# bench all step planning
# ---------------------------------------------------------------------------


class TestPlanSteps:
    def test_debug_variant_algorithm_runs_twice(self):
        steps = plan_steps(["milp"])
        assert [(s.csv_name, s.device_debug) for s in steps] == [
            ("milp_debug.csv", True),
            ("milp.csv", False),
        ]

    def test_single_run_algorithm_runs_once(self):
        steps = plan_steps(["uninstrumented"])
        assert [(s.csv_name, s.device_debug) for s in steps] == [
            ("uninstrumented.csv", False)
        ]

    def test_default_matrix_order(self):
        steps = plan_steps(list(DEFAULT_ALGORITHMS))
        assert len(steps) == 9
        assert steps[0].algorithm == "milp"
        assert steps[-1].csv_name == "uninstrumented.csv"

    def test_every_csv_name_is_unique(self):
        names = [s.csv_name for s in plan_steps(list(ALL_ALGORITHMS))]
        assert len(names) == len(set(names))


class TestAllOk:
    def test_failed_step_is_not_ok(self):
        outcomes = [
            StepOutcome("milp", Path("milp.csv"), "ok", ""),
            StepOutcome("chunked", Path("chunked.csv"), "failed", "boom"),
        ]
        assert not all_ok(outcomes)

    def test_skipped_step_is_ok(self):
        outcomes = [StepOutcome("milp", Path("milp.csv"), "skipped", "")]
        assert all_ok(outcomes)
