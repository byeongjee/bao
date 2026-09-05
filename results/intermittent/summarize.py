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
    "sha256",
    "stringsearch",
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
    "wait_time_us",
    "saleae_wait_count",
    "saleae_wait_deaths",
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

# ---------------------------------------------------------------------------
# Decomposition: T_total = T_exec + T_checkpoint + T_recharge + T_rest
#   T_recharge   = wait_time_us (Saleae wait channel, outages included)
#   T_exec       = uninstrumented O3 under continuous power (continuous/uninstrumented.csv)
#   T_checkpoint = continuous-power time of the same wait-mode binary at the
#                  board cap (continuous/<algo>.csv) minus T_exec: state saves
#                  plus the voltage check at every boundary
#   T_rest       = remainder: boot + restore after real power failures, wait
#                  entry/exit
# ---------------------------------------------------------------------------


def read_bench_times(path):
    """benchmark -> execution time (s) from a `ckpt bench` CSV."""
    if not path.is_file():
        return {}
    with open(path, newline="") as f:
        return {
            r["benchmark"].split("-")[0]: float(r["execution_time_us"]) / 1e6
            for r in csv.DictReader(f)
            if r["status"] == "ok" and r["execution_time_us"]
        }


COMPONENTS = ["t_exec", "t_checkpoint", "t_recharge", "t_rest"]
t_uninstr = read_bench_times(OUT / "continuous" / "uninstrumented.csv")
t_cont = {a: read_bench_times(OUT / "continuous" / f"{a}.csv") for a in ALGOS}

decomp = []
for r in rows:
    b, a = r["benchmark"], r["algorithm"]
    if r["status"] != "ok" or not r.get("wait_time_us"):
        continue
    if b not in t_uninstr or b not in t_cont[a]:
        continue
    total = secs(r)
    recharge = float(r["wait_time_us"]) / 1e6
    exec_ = t_uninstr[b]
    checkpoint = t_cont[a][b] - exec_
    decomp.append(
        {
            "benchmark": b,
            "algorithm": a,
            "trace": r["trace"],
            "t_total": total,
            "t_exec": exec_,
            "t_checkpoint": checkpoint,
            "t_recharge": recharge,
            "t_rest": total - recharge - t_cont[a][b],
        }
    )

if decomp:
    with open(OUT / "decomposition.csv", "w", newline="") as f:
        w = csv.DictWriter(
            f, fieldnames=["benchmark", "algorithm", "trace", "t_total"] + COMPONENTS
        )
        w.writeheader()
        w.writerows(decomp)

    bao_total = {
        (d["benchmark"], d["trace"]): d["t_total"]
        for d in decomp
        if d["algorithm"] == "milp"
    }

    def mean(xs):
        return sum(xs) / len(xs)

    def gmean(xs):
        import math

        return math.exp(mean([math.log(x) for x in xs]))

    def per_algo(ds):
        """Mean share of each component, and each component normalized to
        Bao's total on the same (benchmark, trace): mean over traces, then
        over benchmarks, so the normalized parts add up to the normalized
        total. The geomean total matches the figure."""
        out = {}
        for a in ALGOS:
            da = [
                d
                for d in ds
                if d["algorithm"] == a and (d["benchmark"], d["trace"]) in bao_total
            ]
            if not da:
                continue
            benches = sorted({d["benchmark"] for d in da})
            norm = {}
            for c in COMPONENTS + ["t_total"]:
                per_b = [
                    mean(
                        [
                            d[c] / bao_total[(d["benchmark"], d["trace"])]
                            for d in da
                            if d["benchmark"] == b
                        ]
                    )
                    for b in benches
                ]
                norm[c] = mean(per_b)
            share = {c: mean([d[c] / d["t_total"] for d in da]) for c in COMPONENTS}
            gm = gmean(
                [
                    gmean(
                        [
                            d["t_total"] / bao_total[(d["benchmark"], d["trace"])]
                            for d in da
                            if d["benchmark"] == b
                        ]
                    )
                    for b in benches
                ]
            )
            out[a] = {"norm": norm, "share": share, "gmean_total": gm, "n": len(da)}
        return out

    summary_algo = per_algo(decomp)
    L += [
        "## Decomposition (normalized to Bao's total on the same trace; mean over traces, then benchmarks)",
        "",
        "| algo | runs | total (arith) | total (geomean) | "
        + " | ".join(COMPONENTS)
        + " | power-off share |",
        "|---|---|---|---|" + "---|" * (len(COMPONENTS) + 1),
    ]
    for a, v in summary_algo.items():
        L.append(
            f"| {a} | {v['n']} | {v['norm']['t_total']:.2f} | {v['gmean_total']:.2f} | "
            + " | ".join(
                f"{v['norm'][c]:.3f} ({100 * v['share'][c]:.1f}%)" for c in COMPONENTS
            )
            + f" | {100 * v['share']['t_recharge']:.1f}% |"
        )
    L += [
        "",
        "## Decomposition per benchmark (mean share of each run's total: exec / ckpt / recharge / rest)",
        "",
    ]
    L += ["| benchmark | " + " | ".join(ALGOS) + " |", "|---|" + "---|" * len(ALGOS)]
    for b in BENCHMARKS:
        cells = []
        for a in ALGOS:
            da = [d for d in decomp if d["benchmark"] == b and d["algorithm"] == a]
            if not da:
                cells.append("—")
                continue
            cells.append(
                " / ".join(
                    f"{100 * mean([d[c] / d['t_total'] for d in da]):.0f}%"
                    for c in COMPONENTS
                )
            )
        L.append(f"| {b} | " + " | ".join(cells) + " |")

    # LaTeX table for the paper: one row per system.
    NAMES = {
        "milp": r"\tool",
        "rockclimb": r"\rockclimb",
        "schematic": r"\schematic",
        "schematicO3": r"\schematicO",
    }
    T = [
        r"\begin{tabular}{lrrrrrr}",
        r"\toprule",
        r"System & $T_{\mathrm{total}}$ & $T_{\mathrm{exec}}$ & $T_{\mathrm{ckpt}}$ & $T_{\mathrm{recharge}}$ & $T_{\mathrm{boot+restore}}$ & power-off \\",
        r"\midrule",
    ]
    for a, v in summary_algo.items():
        T.append(
            f"{NAMES[a]} & {v['norm']['t_total']:.2f} & "
            + " & ".join(f"{v['norm'][c]:.2f}" for c in COMPONENTS)
            + f" & {100 * v['share']['t_recharge']:.0f}\\% \\\\"
        )
    T += [r"\bottomrule", r"\end{tabular}"]
    (OUT / "decomposition.tex").write_text("\n".join(T) + "\n")

(OUT / "report.md").write_text("\n".join(L) + "\n")
print(f"{len(rows)} rows -> summary.csv, report.md; {len(decomp)} decomposed rows")
