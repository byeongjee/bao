"""Regression tests for source loops split into nested loops by LLVM."""

from __future__ import annotations

import subprocess

import pytest

pytestmark = pytest.mark.milp


SPLIT_LOOP_IR = r"""
define i32 @split_loop(i1 %take.inner, i1 %repeat.inner) {
entry:
  br label %outer

outer:
  %value = phi i32 [ 0, %entry ], [ %next, %outer.latch ]
  %keep.going = icmp slt i32 %value, 3
  br label %inner

inner:
  br i1 %take.inner, label %inner.latch, label %outer.latch

inner.latch:
  br i1 %repeat.inner, label %inner, label %exit, !llvm.loop !0

outer.latch:
  %next = add i32 %value, 1
  br i1 %keep.going, label %outer, label %exit, !llvm.loop !0

exit:
  %result = phi i32 [ %value, %inner.latch ], [ %next, %outer.latch ]
  ret i32 %result
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.tripcount.upper", i64 10}
"""


def test_unifies_split_loop_without_synthesizing_inner_phis(tools):
    transformed = subprocess.run(
        [
            tools["opt"],
            f"-load-pass-plugin={tools['pass_lib']}",
            "-passes=milp-unify-annotated-loops,verify",
            "-S",
            "-o",
            "-",
        ],
        input=SPLIT_LOOP_IR,
        capture_output=True,
        text=True,
        check=False,
    )
    assert transformed.returncode == 0, transformed.stderr
    assert transformed.stdout.count("!llvm.loop !") == 1

    loops = subprocess.run(
        [tools["opt"], "-passes=print<loops>", "-disable-output"],
        input=transformed.stdout,
        capture_output=True,
        text=True,
        check=False,
    )
    assert loops.returncode == 0, loops.stderr
    assert "Loop at depth 1" in loops.stderr
    assert "Loop at depth 2" not in loops.stderr
