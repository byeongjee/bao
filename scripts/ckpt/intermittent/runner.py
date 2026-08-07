"""Benchmark runner for programs powered by a replayed harvesting trace.

``bench`` measures a program under continuous power. This path instead drives
the target from an Otii Ace replaying a recorded harvester trace, so the
program actually loses power and recovers. Every timing column therefore means
wall-clock time including outages, which is why the CSV is kept separate from
the bench one rather than reusing it.

Skeleton: the compile/replay/measure loop is not written yet.
"""

from __future__ import annotations

import logging
from pathlib import Path

from ..env import ProjectEnv
from ..toolchain import Toolchain

logger = logging.getLogger(__name__)


def run_intermittent_benchmarks(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    algorithm: str,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    traces: list[Path],
    output_csv: Path | None,
    pass_log_level: str,
) -> None:
    """Run *algorithm* over (benchmark x capacitor x trace) under replayed power."""
    raise NotImplementedError(
        f"ckpt intermittent {algorithm}: replay loop not implemented "
        f"({len(traces)} trace(s) requested)"
    )
