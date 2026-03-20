"""Tests for EdgeSplitPass: verifies merge-point edges are split."""

import re
import subprocess

import pytest

pytestmark = pytest.mark.milp


def _run_opt(opt_bin: str, plugin: str, input_ir: str, passes: str) -> str:
    """Run opt with the given passes and return the output IR."""
    result = subprocess.run(
        [opt_bin, f"-load-pass-plugin={plugin}", f"-passes={passes}",
         "-S", "-o", "-"],
        input=input_ir,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, f"opt failed: {result.stderr}"
    return result.stdout


def _get_merge_point_pred_pred_counts(ir_text: str) -> list[tuple[str, str, int]]:
    """For each merge-point block, return (merge_block, pred, pred_pred_count).

    Parses branch targets from IR to build a predecessor map, then checks
    the edge-split invariant: every predecessor of a merge point should
    have exactly one predecessor.
    """
    # Build predecessor map from branch targets
    block_labels = re.findall(r"^(\S+):", ir_text, re.MULTILINE)
    preds: dict[str, list[str]] = {label: [] for label in block_labels}

    # For each block, find its branch targets
    current_block = None
    for line in ir_text.splitlines():
        label_match = re.match(r"^(\S+):", line)
        if label_match:
            current_block = label_match.group(1)
            continue
        if current_block is None:
            continue
        for target in re.findall(r"label\s+%(\S+)", line):
            target = target.rstrip(",")
            if target in preds:
                preds[target].append(current_block)

    # Check merge points
    results = []
    for block, block_preds in preds.items():
        if len(block_preds) <= 1:
            continue
        for pred in block_preds:
            pred_pred_count = len(preds.get(pred, []))
            results.append((block, pred, pred_pred_count))
    return results


DIAMOND_IR = """\
define i32 @diamond(i32 %x) {
entry:
  %cmp = icmp sgt i32 %x, 0
  br i1 %cmp, label %if.then, label %if.else

if.then:
  %a = add i32 %x, 1
  br label %if.end

if.else:
  %b = sub i32 %x, 1
  br label %if.end

if.end:
  %result = phi i32 [ %a, %if.then ], [ %b, %if.else ]
  ret i32 %result
}
"""


def test_diamond_merge_point_preds_have_single_pred(tools):
    """After EdgeSplit, predecessors of merge points each have 1 predecessor."""
    out = _run_opt(tools["opt"], tools["pass_lib"], DIAMOND_IR, "milp-preprocess")

    violations = _get_merge_point_pred_pred_counts(out)
    for merge_block, pred, pred_pred_count in violations:
        assert pred_pred_count == 1, (
            f"Merge point %{merge_block}: predecessor %{pred} has "
            f"{pred_pred_count} predecessors (expected 1)"
        )
    # Verify there ARE merge points (the diamond's if.end should still exist)
    merge_points = [b for b, p, c in violations]
    assert len(merge_points) > 0, (
        "Expected at least one merge point in diamond CFG after edge splitting"
    )
