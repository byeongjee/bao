#!/usr/bin/env python3
"""Run all benchmark configurations and save results to result/.

Usage:
    uv run python scripts/run_benchmarks.py [BENCHMARKS...]
    uv run python scripts/run_benchmarks.py test aes crc rsa
    uv run python scripts/run_benchmarks.py          # discovers all benchmarks
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ALGORITHMS_WITH_DEBUG = ["milp", "schematic", "rockclimb", "schematicO3"]
RESULT_DIR = Path("result")


def run(cmd: list[str]) -> None:
    print(f"\n>>> {' '.join(cmd)}")
    subprocess.check_call(cmd)


def main() -> None:
    benchmarks = sys.argv[1:]
    RESULT_DIR.mkdir(exist_ok=True)

    for algo in ALGORITHMS_WITH_DEBUG:
        print(f"\n=== {algo} (with device-debug) ===")
        run(
            ["uv", "run", "ckpt", "bench", algo]
            + benchmarks
            + ["-o", str(RESULT_DIR / f"{algo}-swbor.csv")]
        )
        print(f"\n=== {algo} (no device-debug) ===")
        run(
            ["uv", "run", "ckpt", "bench", algo]
            + benchmarks
            + ["--no-device-debug", "-o", str(RESULT_DIR / f"{algo}-swbor-no-debug.csv")]
        )

    print("\n=== uninstrumented ===")
    run(
        ["uv", "run", "ckpt", "bench", "uninstrumented"]
        + benchmarks
        + ["-o", str(RESULT_DIR / "uninstrumented.csv")]
    )

    print(f"\nDone. Results in {RESULT_DIR}/")


if __name__ == "__main__":
    main()
