"""Merge results/intermittent_v2/<bench>/<algo>.csv into summary.csv and report.md."""

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
    "runtime_wait_count",
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


def secs(r):
    return float(r["execution_time_us"]) / 1e6


def cell(r):
    if r is None:
        return "—"
    if r["status"] != "ok":
        return r["status"]
    mark = "" if r["correct"] == "yes" else " WRONG"
    return f"ok{mark} w={r['runtime_wait_count']} rec={r['runtime_recovery_boots']} t={secs(r):.2f}s"


L = [
    "# Intermittent-power results (v2)",
    "",
    (
        "13 benchmarks x 4 algorithms x 10 traces (benchmarks/traces/1..10.csv replayed "
        "10x faster than recorded), board cap 11.5 uF, 560 Ohm series resistor, wait "
        "threshold 3.0 V, halt-mode wait, cpu 16 MHz, no device-debug."
    ),
    "",
    (
        "Cell: `ok` = completed (stop pulse + __nvm_done) and return code equals the "
        "uninstrumented baseline (`WRONG` otherwise); `incomplete` = did not finish within "
        "the trace; w = waits (cnt_wait: boundary/boot found VCC below 3.0 V and slept); "
        "rec = recovery boots (cnt_recovery, real brownouts); t = execution time incl. "
        "wait/outage time (Saleae start->stop pulse)."
    ),
    "",
    "## Totals per algorithm",
    "",
    "| algo | runs | complete | correct (of complete) | incomplete | mean waits | mean rec | mean t (s) |",
    "|---|---|---|---|---|---|---|---|",
]
for a in ALGOS:
    rs = [r for r in rows if r["algorithm"] == a]
    ok = [r for r in rs if r["status"] == "ok"]
    corr = sum(r["correct"] == "yes" for r in ok)
    ws = [int(r["runtime_wait_count"]) for r in ok]
    recs = [int(r["runtime_recovery_boots"]) for r in ok]
    ts = [secs(r) for r in ok]
    L.append(
        f"| {a} | {len(rs)} | {len(ok)} | {corr} | {len(rs) - len(ok)} | "
        f"{sum(ws) / len(ws):.1f} | {sum(recs) / len(recs):.1f} | {sum(ts) / len(ts):.2f} |"
    )
L += [
    "",
    "## Per benchmark: complete / correct / total waits / total rec / mean t (s)",
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
        wsum = sum(int(r["runtime_wait_count"]) for r in ok)
        rec = sum(int(r["runtime_recovery_boots"]) for r in ok)
        ts = [secs(r) for r in ok]
        mt = f"{sum(ts) / len(ts):.2f}" if ts else "-"
        cells.append(f"{len(ok)}/{len(rs)} / {corr}/{len(ok)} / {wsum} / {rec} / {mt}")
    L.append(f"| {b} ({baseline.get(b, '?')}) | " + " | ".join(cells) + " |")

L += ["", "## Per trace: waits by benchmark x algorithm (`inc` = incomplete)", ""]
for t in TRACES:
    L += [
        f"### trace {t}",
        "",
        "| benchmark | " + " | ".join(ALGOS) + " |",
        "|---|" + "---|" * len(ALGOS),
    ]
    for b in BENCHMARKS:
        vals = []
        for a in ALGOS:
            r = by.get((b, a, t))
            vals.append(
                "—"
                if r is None
                else (r["runtime_wait_count"] if r["status"] == "ok" else "inc")
            )
        L.append(f"| {b} | " + " | ".join(vals) + " |")
    L.append("")

L += ["## Per benchmark x algorithm x trace", ""]
for b in BENCHMARKS:
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
