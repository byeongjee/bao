"""Parse loop bound annotations from LLVM IR.

Supports annotations in the form:
    __attribute__((annotate("loop_bound:N"))) volatile int _marker = N;

These appear in LLVM IR as global strings in the llvm.metadata section.
"""

import re
from dataclasses import dataclass
from pathlib import Path


@dataclass
class LoopBound:
    """A loop bound annotation."""
    bound: int
    source_line: int | None = None


def parse_loop_bounds_from_ir(ir_path: Path) -> dict[str, int]:
    """Parse loop bound annotations from LLVM IR file.

    Returns a dict mapping annotation strings to their bound values.
    These can be used to override default loop iteration limits.
    """
    bounds: dict[str, int] = {}

    # Pattern matches: @.str = ... c"loop_bound:N\00"
    pattern = re.compile(r'c"loop_bound:(\d+)\\00"')

    with open(ir_path) as f:
        for line in f:
            match = pattern.search(line)
            if match:
                bound = int(match.group(1))
                bounds[f"loop_bound:{bound}"] = bound

    return bounds


def parse_loop_bounds_from_source(c_path: Path) -> list[LoopBound]:
    """Parse loop bound annotations from C source comments.

    Supports comment formats:
        /* LOOP_BOUND: N */
        // LOOP_BOUND: N
        /* @loop_bound(N) */

    Returns list of LoopBound with source line numbers.
    """
    bounds: list[LoopBound] = []

    patterns = [
        re.compile(r'/\*\s*LOOP_BOUND:\s*(\d+)\s*\*/'),
        re.compile(r'//\s*LOOP_BOUND:\s*(\d+)'),
        re.compile(r'/\*\s*@loop_bound\((\d+)\)\s*\*/'),
        re.compile(r'LOOP_BOUND\((\d+)\)'),  # Macro usage
    ]

    with open(c_path) as f:
        for line_num, line in enumerate(f, 1):
            for pattern in patterns:
                match = pattern.search(line)
                if match:
                    bound = int(match.group(1))
                    bounds.append(LoopBound(bound=bound, source_line=line_num))
                    break  # Only one bound per line

    return bounds


def get_max_loop_bound(ir_path: Path | None = None, c_path: Path | None = None) -> int | None:
    """Get the maximum loop bound from annotations.

    Useful as an overall bound when per-loop bounds aren't mapped.
    """
    bounds: list[int] = []

    if ir_path and ir_path.exists():
        ir_bounds = parse_loop_bounds_from_ir(ir_path)
        bounds.extend(ir_bounds.values())

    if c_path and c_path.exists():
        source_bounds = parse_loop_bounds_from_source(c_path)
        bounds.extend(b.bound for b in source_bounds)

    return max(bounds) if bounds else None


def get_all_loop_bounds(ir_path: Path | None = None, c_path: Path | None = None) -> list[int]:
    """Get all unique loop bounds from annotations, sorted."""
    bounds: set[int] = set()

    if ir_path and ir_path.exists():
        ir_bounds = parse_loop_bounds_from_ir(ir_path)
        bounds.update(ir_bounds.values())

    if c_path and c_path.exists():
        source_bounds = parse_loop_bounds_from_source(c_path)
        bounds.update(b.bound for b in source_bounds)

    return sorted(bounds)
