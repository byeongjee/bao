#!/usr/bin/env python3
"""Parse output.log from benchmark_milp.sh --verbose to extract
the relationship between capacitor size and loop strip-mining K values."""

import re
import csv
import sys

CAPACITY_MAP = {
    "100nF": 486.0,
    "1uF": 4860.0,
    "10uF": 48600.0,
    "100uF": 486000.0,
}

def parse_log(log_path):
    with open(log_path) as f:
        text = f.read()

    # Split into per-run blocks by the "[N/M] Running ..." header
    run_pattern = re.compile(
        r'\[(\d+)/(\d+)\] Running (\S+) \.\.\.'
    )

    runs = []
    headers = list(run_pattern.finditer(text))
    for i, m in enumerate(headers):
        start = m.start()
        end = headers[i + 1].start() if i + 1 < len(headers) else len(text)
        block = text[start:end]

        benchmark = m.group(3)  # e.g. "activity_recognition-100nF"
        # Split into program and capacitor
        parts = benchmark.rsplit("-", 1)
        program = parts[0]
        capacitor = parts[1] if len(parts) > 1 else ""
        capacity = CAPACITY_MAP.get(capacitor, 0)

        # Extract loop strip-mining stats
        innermost = 0
        eligible = 0
        rewritten = 0
        skipped = 0
        skipped_reasons = {}
        chosen_k = {}

        chunking_match = re.search(r'=== Loop Strip-Mining: (\S+) ===', block)
        if chunking_match:
            m2 = re.search(r'Loops considered:\s+(\d+)', block)
            if m2:
                innermost = int(m2.group(1))
            m2 = re.search(r'Eligible loops:\s+(\d+)', block)
            if m2:
                eligible = int(m2.group(1))
            m2 = re.search(r'Rewritten loops:\s+(\d+)', block)
            if m2:
                rewritten = int(m2.group(1))
            m2 = re.search(r'Skipped loops:\s+(\d+)', block)
            if m2:
                skipped = int(m2.group(1))

            # Parse skip reasons (before "Chosen K values" section)
            k_section_start = block.find("Chosen K values:")
            reason_section = block[:k_section_start] if k_section_start != -1 else block
            for reason_m in re.finditer(r'- ([\w-]+):\s+(\d+)', reason_section):
                reason = reason_m.group(1)
                count = int(reason_m.group(2))
                if reason in ("k-covers-entire-loop", "k-not-beneficial",
                              "missing-canonical-induction"):
                    skipped_reasons[reason] = count

            # Parse chosen K values
            if k_section_start != -1:
                # Find all "- loop_name: K=N" after "Chosen K values:"
                k_section = block[k_section_start:]
                # Stop at next section (Set parameter, ===, etc.)
                k_end = re.search(r'\n(?:Set parameter|===)', k_section)
                if k_end:
                    k_section = k_section[:k_end.start()]
                for k_match in re.finditer(r'- ([\w.]+):\s+K=(\d+)', k_section):
                    loop_name = k_match.group(1)
                    k_val = int(k_match.group(2))
                    chosen_k[loop_name] = k_val

        runs.append({
            "program": program,
            "capacitor": capacitor,
            "capacity": capacity,
            "innermost_loops": innermost,
            "eligible_loops": eligible,
            "rewritten_loops": rewritten,
            "skipped_loops": skipped,
            "skipped_reasons": skipped_reasons,
            "chosen_k": chosen_k,
        })

    return runs


def write_csv(runs, out_path):
    # Collect all unique loop names per program
    program_loops = {}
    for r in runs:
        prog = r["program"]
        if prog not in program_loops:
            program_loops[prog] = set()
        program_loops[prog].update(r["chosen_k"].keys())

    # Write one row per (program, capacitor, loop) combination
    rows = []
    for r in runs:
        prog = r["program"]
        cap = r["capacitor"]
        capacity = r["capacity"]
        k_covers = r["skipped_reasons"].get("k-covers-entire-loop", 0)

        if r["chosen_k"]:
            for loop_name, k_val in sorted(r["chosen_k"].items()):
                rows.append({
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
                })
        else:
            # No loops were strip-mined at this capacitor size
            rows.append({
                "program": prog,
                "capacitor": cap,
                "capacity": capacity,
                "loop_name": "(none)",
                "K": "",
                "status": "no_chunking_needed" if k_covers > 0 else "no_eligible_loops",
                "innermost_loops": r["innermost_loops"],
                "eligible_loops": r["eligible_loops"],
                "rewritten_loops": r["rewritten_loops"],
                "skipped_k_covers_entire_loop": k_covers,
                "skipped_other": r["skipped_loops"] - k_covers,
            })

    fieldnames = [
        "program", "capacitor", "capacity", "loop_name", "K", "status",
        "innermost_loops", "eligible_loops", "rewritten_loops",
        "skipped_k_covers_entire_loop", "skipped_other",
    ]

    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    print(f"Wrote {len(rows)} rows to {out_path}")


if __name__ == "__main__":
    log_path = sys.argv[1] if len(sys.argv) > 1 else "output.log"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "loop_strip_mining_vs_capacitor.csv"
    runs = parse_log(log_path)
    write_csv(runs, out_path)
