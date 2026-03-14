"""Strip-mining log parsing and CSV export.

Absorbs scripts/parse_loop_strip_mining.py. Parses verbose MILP benchmark
logs for loop strip-mining K values and writes per-loop CSV output.
"""

from __future__ import annotations

import csv
import logging
import re
from pathlib import Path

logger = logging.getLogger(__name__)

CAPACITY_MAP = {
    "1uF": 4860.0,
    "10uF": 48600.0,
    "100uF": 486000.0,
}

_RUN_HEADER_RE = re.compile(r"\[(\d+)/(\d+)\] Running (\S+) \.\.\.")


def parse_strip_mining_log(log_path: Path) -> list[dict]:
    """Parse verbose MILP benchmark log for strip-mining K values.

    Splits into per-run blocks by ``[N/M] Running ...`` headers.
    Extracts per-run: program, capacitor, capacity, loop counts,
    skip reasons, and chosen K values per loop.
    """
    text = log_path.read_text()

    headers = list(_RUN_HEADER_RE.finditer(text))
    runs: list[dict] = []

    for i, m in enumerate(headers):
        start = m.start()
        end = headers[i + 1].start() if i + 1 < len(headers) else len(text)
        block = text[start:end]

        benchmark = m.group(3)  # e.g. "activity_recognition-100uF"
        parts = benchmark.rsplit("-", 1)
        program = parts[0]
        capacitor = parts[1] if len(parts) > 1 else ""
        capacity = CAPACITY_MAP.get(capacitor, 0.0)

        innermost = 0
        eligible = 0
        rewritten = 0
        skipped = 0
        skipped_reasons: dict[str, int] = {}
        chosen_k: dict[str, int] = {}

        if re.search(r"=== Loop Strip-Mining: \S+ ===", block):
            m2 = re.search(r"Loops considered:\s+(\d+)", block)
            if m2:
                innermost = int(m2.group(1))
            m2 = re.search(r"Eligible loops:\s+(\d+)", block)
            if m2:
                eligible = int(m2.group(1))
            m2 = re.search(r"Rewritten loops:\s+(\d+)", block)
            if m2:
                rewritten = int(m2.group(1))
            m2 = re.search(r"Skipped loops:\s+(\d+)", block)
            if m2:
                skipped = int(m2.group(1))

            # Parse skip reasons (before "Chosen K values" section)
            k_section_start = block.find("Chosen K values:")
            reason_section = block[:k_section_start] if k_section_start != -1 else block
            for reason_m in re.finditer(r"- ([\w-]+):\s+(\d+)", reason_section):
                reason = reason_m.group(1)
                count = int(reason_m.group(2))
                if reason in (
                    "k-covers-entire-loop",
                    "k-not-beneficial",
                    "missing-canonical-induction",
                ):
                    skipped_reasons[reason] = count

            # Parse chosen K values
            if k_section_start != -1:
                k_section = block[k_section_start:]
                k_end = re.search(r"\n(?:Set parameter|===)", k_section)
                if k_end:
                    k_section = k_section[: k_end.start()]
                for k_match in re.finditer(r"- ([\w.]+):\s+K=(\d+)", k_section):
                    loop_name = k_match.group(1)
                    k_val = int(k_match.group(2))
                    chosen_k[loop_name] = k_val

        runs.append(
            {
                "program": program,
                "capacitor": capacitor,
                "capacity": capacity,
                "innermost_loops": innermost,
                "eligible_loops": eligible,
                "rewritten_loops": rewritten,
                "skipped_loops": skipped,
                "skipped_reasons": skipped_reasons,
                "chosen_k": chosen_k,
            }
        )

    return runs


_CSV_FIELDS = [
    "program",
    "capacitor",
    "capacity",
    "loop_name",
    "K",
    "status",
    "innermost_loops",
    "eligible_loops",
    "rewritten_loops",
    "skipped_k_covers_entire_loop",
    "skipped_other",
]


def write_strip_mining_csv(runs: list[dict], output_path: Path) -> None:
    """Write strip-mining analysis to CSV.

    One row per (program, capacitor, loop) combination.
    Columns: program, capacitor, capacity, loop_name, K, status,
    innermost_loops, eligible_loops, rewritten_loops,
    skipped_k_covers_entire_loop, skipped_other
    """
    rows: list[dict] = []

    for r in runs:
        prog = r["program"]
        cap = r["capacitor"]
        capacity = r["capacity"]
        k_covers = r["skipped_reasons"].get("k-covers-entire-loop", 0)

        if r["chosen_k"]:
            for loop_name, k_val in sorted(r["chosen_k"].items()):
                rows.append(
                    {
                        "program": prog,
                        "capacitor": cap,
                        "capacity": capacity,
                        "loop_name": loop_name,
                        "K": k_val,
                        "status": "chunked",
                        "innermost_loops": r["innermost_loops"],
                        "eligible_loops": r["eligible_loops"],
                        "rewritten_loops": r["rewritten_loops"],
                        "skipped_k_covers_entire_loop": k_covers,
                        "skipped_other": r["skipped_loops"] - k_covers,
                    }
                )
        else:
            status = (
                "no_chunking_needed"
                if k_covers > 0
                else "no_eligible_loops"
            )
            rows.append(
                {
                    "program": prog,
                    "capacitor": cap,
                    "capacity": capacity,
                    "loop_name": "(none)",
                    "K": "",
                    "status": status,
                    "innermost_loops": r["innermost_loops"],
                    "eligible_loops": r["eligible_loops"],
                    "rewritten_loops": r["rewritten_loops"],
                    "skipped_k_covers_entire_loop": k_covers,
                    "skipped_other": r["skipped_loops"] - k_covers,
                }
            )

    with output_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=_CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    logger.info("Wrote %d rows to %s", len(rows), output_path)
