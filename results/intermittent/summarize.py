"""Merge results/intermittent/<bench>/<algo>.csv into summary.csv and report.md."""

import csv
from pathlib import Path

OUT = Path(__file__).parent
BENCHMARKS = [
    "aes",
    "crc",
    "rsa",
    "dijkstra",
    "qsort",
    "activity_recognition",
    "bitcount",
    "chacha20",
    "sensor_fusion",
    "poly1305",
    "cuckoo_filter",
    "sha256_fixed",
    "stringsearch_fixed",
]
ALGOS = ["milp", "rockclimb", "schematic", "schematicO3"]
TRACES = [str(i) for i in range(1, 11)]

baseline = {}
if (OUT / "baseline.csv").is_file():
    with open(OUT / "baseline.csv", newline="") as f:
        baseline = {r["benchmark"]: r["result"] for r in csv.DictReader(f)}

rows = []
for b in BENCHMARKS:
    for a in ALGOS:
        p = OUT / b / f"{a}.csv"
        if not p.is_file():
            continue
        with open(p, newline="") as f:
            for r in csv.DictReader(f):
                r["algorithm"] = a
                r["complete"] = "yes" if r["status"] == "ok" else "no"
                ref = baseline.get(b, "")
                r["correct"] = (
                    ("yes" if r["result"] == ref else "no")
                    if r["status"] == "ok" and ref
                    else ""
                )
                r["expected_result"] = ref
                rows.append(r)

cols = [
    "benchmark",
    "algorithm",
    "trace",
    "status",
    "complete",
    "correct",
    "result",
    "expected_result",
    "runtime_recovery_boots",
    "execution_time_us",
    "replay_seconds",
    "region_boundaries",
]
with open(OUT / "summary.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
    w.writeheader()
    w.writerows(rows)

by = {(r["benchmark"], r["algorithm"], r["trace"]): r for r in rows}


def cell(r):
    if r is None:
        return "—"
    if r["status"] != "ok":
        return r["status"]
    t = f"{float(r['execution_time_us']) / 1e6:.2f}s" if r["execution_time_us"] else "?"
    mark = "" if r["correct"] == "yes" else " WRONG"
    return f"ok{mark} rec={r['runtime_recovery_boots']} t={t}"


L = [
    "# Intermittent-power results",
    "",
    (
        "13 benchmarks x 4 algorithms x 10 traces (benchmarks/traces/1..10.csv), board cap (11.5 uF), "
        "halt-mode wait, cpu 16 MHz, no device-debug."
    ),
    "",
    (
        "Cell: `ok` = completed (stop pulse + __nvm_done) and return code equals the uninstrumented baseline "
        "(`WRONG` otherwise); `incomplete` = did not finish within the trace; "
        "rec = recovery boots (cnt_recovery); t = execution time incl. wait/outage time (Saleae start->stop pulse)."
    ),
    "",
]

L += [
    "## Totals per algorithm",
    "",
    "| algo | runs | complete | correct (of complete) | incomplete/other | mean rec (complete) | mean t (complete, s) |",
    "|---|---|---|---|---|---|---|",
]
for a in ALGOS:
    rs = [r for r in rows if r["algorithm"] == a]
    if not rs:
        continue
    ok = [r for r in rs if r["status"] == "ok"]
    corr = sum(r["correct"] == "yes" for r in ok)
    recs = [int(r["runtime_recovery_boots"]) for r in ok if r["runtime_recovery_boots"]]
    ts = [float(r["execution_time_us"]) / 1e6 for r in ok if r["execution_time_us"]]
    L.append(
        f"| {a} | {len(rs)} | {len(ok)} | {corr} | {len(rs) - len(ok)} | "
        f"{sum(recs) / len(recs):.1f} | {sum(ts) / len(ts):.2f} |"
    )
L.append("")

L += [
    "## Per benchmark: complete / correct / total rec / mean t (s)",
    "",
    "| benchmark (baseline result) | " + " | ".join(ALGOS) + " |",
    "|---|" + "---|" * len(ALGOS),
]
for b in BENCHMARKS:
    cells = []
    for a in ALGOS:
        rs = [by[(b, a, t)] for t in TRACES if (b, a, t) in by]
        ok = [r for r in rs if r["status"] == "ok"]
        if not rs:
            cells.append("—")
            continue
        corr = sum(r["correct"] == "yes" for r in ok)
        rec = sum(
            int(r["runtime_recovery_boots"]) for r in ok if r["runtime_recovery_boots"]
        )
        ts = [float(r["execution_time_us"]) / 1e6 for r in ok if r["execution_time_us"]]
        mt = f"{sum(ts) / len(ts):.2f}" if ts else "-"
        cells.append(f"{len(ok)}/{len(rs)} / {corr}/{len(ok)} / {rec} / {mt}")
    L.append(f"| {b} ({baseline.get(b, '?')}) | " + " | ".join(cells) + " |")
L.append("")

L += ["## Per benchmark x algorithm x trace", ""]
for b in BENCHMARKS:
    if not any(k[0] == b for k in by):
        continue
    L += [
        f"### {b} (baseline result {baseline.get(b, '?')})",
        "",
        "| algo | " + " | ".join(f"tr{t}" for t in TRACES) + " |",
        "|---|" + "---|" * len(TRACES),
    ]
    for a in ALGOS:
        L.append(
            f"| {a} | " + " | ".join(cell(by.get((b, a, t))) for t in TRACES) + " |"
        )
    L.append("")
(OUT / "report.md").write_text("\n".join(L) + "\n")
print(f"{len(rows)} rows -> summary.csv, report.md")
