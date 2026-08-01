"""Unit tests for --cap expansion and the verify-all combined report."""

import pytest
from ckpt.cli import _expand_caps
from ckpt.verify.all import format_report
from ckpt.verify.common import BenchResult, Status, all_ok

pytestmark = pytest.mark.unit


def _result(name, cap, status, detail=""):
    return BenchResult(
        name,
        cap,
        status,
        detail,
        baseline_result="0x1234" if status is not Status.ERROR else None,
        algorithm_result="0x1234" if status is Status.PASS else None,
    )


class TestExpandCaps:
    def test_passthrough(self):
        assert _expand_caps(("1uF", "10uF")) == ("1uF", "10uF")

    def test_comma_separated(self):
        assert _expand_caps(("5uF,10uF,50uF",)) == ("5uF", "10uF", "50uF")

    def test_bare_numbers_get_uf_suffix(self):
        assert _expand_caps(("5,10,50",)) == ("5uF", "10uF", "50uF")

    def test_mixed_repeat_and_comma(self):
        assert _expand_caps(("1uF", "5,10uF")) == ("1uF", "5uF", "10uF")

    def test_decimal_bare_number(self):
        assert _expand_caps(("0.5",)) == ("0.5uF",)

    def test_empty(self):
        assert _expand_caps(()) == ()


class TestAllOk:
    def test_pass_and_skip_are_ok(self):
        results = [
            _result("aes", "5uF", Status.PASS),
            _result("crc", "5uF", Status.SKIP, "MILP infeasible"),
        ]
        assert all_ok(results)

    def test_fail_is_not_ok(self):
        assert not all_ok([_result("aes", "5uF", Status.FAIL, "mismatch")])

    def test_error_is_not_ok(self):
        assert not all_ok([_result("aes", "5uF", Status.ERROR, "no device")])


class TestFormatReport:
    def test_matrix_and_counts(self):
        results = {
            "milp": [
                _result("aes", "5uF", Status.PASS),
                _result("aes", "10uF", Status.PASS),
            ],
            "rockclimb": [
                _result("aes", "5uF", Status.FAIL, "baseline=1 rockclimb=2"),
                _result("aes", "10uF", Status.SKIP, "infeasible"),
            ],
        }
        report = format_report(results, "bor")

        assert "Halt mode: bor | Capacitors: 5uF, 10uF" in report
        assert "milp: 2/2 PASSED, 0 FAILED, 0 SKIPPED, 0 ERRORS" in report
        assert "rockclimb: 0/2 PASSED, 1 FAILED, 1 SKIPPED, 0 ERRORS" in report
        assert "FAIL: rockclimb aes [5uF]: baseline=1 rockclimb=2" in report
        assert "SKIP: rockclimb aes [10uF]: infeasible" in report

        matrix_row = next(
            line
            for line in report.splitlines()
            if line.startswith("aes") and "5uF" in line
        )
        assert "PASS" in matrix_row
        assert "FAIL" in matrix_row
