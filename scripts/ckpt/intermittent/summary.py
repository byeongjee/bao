"""Merge the per-(benchmark, algorithm) intermittent CSVs of a result
directory into summary.csv, and decompose each run's time into
decomposition.csv plus the paper's LaTeX tables.

Decomposition: T_total = T_exec + T_checkpoint + T_recharge + T_rest
  T_recharge   = wait_time_us (Saleae wait channel, outages included)
  T_exec       = uninstrumented O3 under continuous power (continuous/uninstrumented.csv)
  T_checkpoint = continuous-power time of the same wait-mode binary at the
                 board cap (continuous/<algo>.csv) minus T_exec: state saves
                 plus the voltage check at every boundary
  T_rest       = remainder: boot + restore after real power failures, wait
                 entry/exit
"""

from __future__ import annotations

import csv
import math
from pathlib import Path

SUMMARY_COLUMNS = [
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

COMPONENTS = ["t_exec", "t_checkpoint", "t_recharge", "t_rest"]

LATEX_NAMES = {
    "milp": r"\tool",
    "rockclimb": r"\rockclimb",
    "schematic": r"\schematic",
    "schematicO3": r"\schematicO",
}


def _mean(xs: list[float]) -> float:
    return sum(xs) / len(xs)


def _gmean(xs: list[float]) -> float:
    return math.exp(_mean([math.log(x) for x in xs]))


def _secs(row: dict[str, str]) -> float:
    return float(row["execution_time_us"]) / 1e6


def _read_bench_times(path: Path) -> dict[str, float]:
    """benchmark -> execution time (s) from a `ckpt bench` CSV."""
    if not path.is_file():
        return {}
    with open(path, newline="") as f:
        return {
            r["benchmark"].split("-")[0]: float(r["execution_time_us"]) / 1e6
            for r in csv.DictReader(f)
            if r["status"] == "ok" and r["execution_time_us"]
        }


def _read_runs(
    result_dir: Path, benchmarks: list[str], algorithms: list[str]
) -> list[dict[str, str]]:
    """Rows of every ``<bench>/<algo>.csv``, marked complete and correct
    against the baseline results in baseline.csv."""
    baseline: dict[str, str] = {}
    if (result_dir / "baseline.csv").is_file():
        with open(result_dir / "baseline.csv", newline="") as f:
            baseline = {r["benchmark"]: r["result"] for r in csv.DictReader(f)}

    rows = []
    for b in benchmarks:
        for a in algorithms:
            p = result_dir / b / f"{a}.csv"
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
    return rows


def _decompose(
    result_dir: Path, rows: list[dict[str, str]], algorithms: list[str]
) -> list[dict]:
    t_uninstr = _read_bench_times(result_dir / "continuous" / "uninstrumented.csv")
    t_cont = {
        a: _read_bench_times(result_dir / "continuous" / f"{a}.csv") for a in algorithms
    }
    decomp = []
    for r in rows:
        b, a = r["benchmark"], r["algorithm"]
        if r["status"] != "ok" or not r.get("wait_time_us"):
            continue
        if b not in t_uninstr or b not in t_cont[a]:
            continue
        total = _secs(r)
        recharge = float(r["wait_time_us"]) / 1e6
        exec_ = t_uninstr[b]
        decomp.append(
            {
                "benchmark": b,
                "algorithm": a,
                "trace": r["trace"],
                "t_total": total,
                "t_exec": exec_,
                "t_checkpoint": t_cont[a][b] - exec_,
                "t_recharge": recharge,
                "t_rest": total - recharge - t_cont[a][b],
            }
        )
    return decomp


def _per_algo(decomp: list[dict], algorithms: list[str]) -> dict[str, dict]:
    """Mean share of each component of each run's own total, and the geomean
    of the total normalized to Bao's total on the same (benchmark, trace):
    over traces, then over benchmarks, as in the figure."""
    bao_total = {
        (d["benchmark"], d["trace"]): d["t_total"]
        for d in decomp
        if d["algorithm"] == "milp"
    }
    out = {}
    for a in algorithms:
        da = [
            d
            for d in decomp
            if d["algorithm"] == a and (d["benchmark"], d["trace"]) in bao_total
        ]
        if not da:
            continue
        benches = sorted({d["benchmark"] for d in da})
        share = {c: _mean([d[c] / d["t_total"] for d in da]) for c in COMPONENTS}
        gm = _gmean(
            [
                _gmean(
                    [
                        d["t_total"] / bao_total[(d["benchmark"], d["trace"])]
                        for d in da
                        if d["benchmark"] == b
                    ]
                )
                for b in benches
            ]
        )
        out[a] = {"share": share, "gmean_total": gm}
    return out


def _pct(x: float) -> str:
    return r"$<0.1$" if abs(x) < 0.0005 else f"{100 * x:.1f}"


def _decomposition_table(summary_algo: dict[str, dict]) -> str:
    """One row per system. Total is the geomean ratio to Bao (as in the
    figure); the parts are mean shares of each run's own total, so they add
    up to 100%."""
    t = [
        r"\begin{tabular}{lrrrrr}",
        r"\toprule",
        r"& & \multicolumn{3}{c}{power-on (\%)} & power-off (\%) \\",
        r"\cmidrule(lr){3-5} \cmidrule(lr){6-6}",
        (
            r"System & $T_{\mathrm{total}}$ & $T_{\mathrm{exec}}$ & $T_{\mathrm{ckpt}}$ & "
            r"$T_{\mathrm{boot+restore}}$ & $T_{\mathrm{recharge}}$ \\"
        ),
        r"\midrule",
    ]
    for a, v in summary_algo.items():
        sh = v["share"]
        t.append(
            f"{LATEX_NAMES[a]} & {v['gmean_total']:.1f}$\\times$ & {_pct(sh['t_exec'])} & "
            f"{_pct(sh['t_checkpoint'])} & {_pct(sh['t_rest'])} & {_pct(sh['t_recharge'])} \\\\"
        )
    t += [r"\bottomrule", r"\end{tabular}"]
    return "\n".join(t) + "\n"


def _per_benchmark_table(
    decomp: list[dict], benchmarks: list[str], algorithms: list[str]
) -> str:
    """Per benchmark, mean share of each run's total as exec / ckpt /
    recharge (boot+restore is the remainder)."""
    t = [
        r"\begin{tabular}{l" + "r" * len(algorithms) + "}",
        r"\toprule",
        "Bench. & " + " & ".join(LATEX_NAMES[a] for a in algorithms) + r" \\",
        r"\midrule",
    ]
    for b in benchmarks:
        cells = []
        for a in algorithms:
            da = [d for d in decomp if d["benchmark"] == b and d["algorithm"] == a]
            cells.append(
                " / ".join(
                    f"{100 * _mean([d[c] / d['t_total'] for d in da]):.1f}"
                    for c in ["t_exec", "t_checkpoint", "t_recharge"]
                )
                if da
                else "---"
            )
        t.append(b.replace("_", r"\_") + " & " + " & ".join(cells) + r" \\")
    t += [r"\bottomrule", r"\end{tabular}"]
    return "\n".join(t) + "\n"


def _speedups(rows: list[dict[str, str]], baseline: str) -> list[list[float]]:
    """Per benchmark, *baseline*'s time over Bao's on each trace where both
    completed, as in the figure."""
    ok = {
        (r["benchmark"], r["algorithm"], r["trace"]): _secs(r)
        for r in rows
        if r["status"] == "ok"
    }
    per_bench: dict[str, list[float]] = {}
    for (b, a, t), secs in ok.items():
        if a == baseline and (b, "milp", t) in ok:
            per_bench.setdefault(b, []).append(secs / ok[(b, "milp", t)])
    return list(per_bench.values())


def _numbers(
    rows: list[dict[str, str]], algorithms: list[str], decomp: list[dict]
) -> list[str]:
    """The paper's in-text numbers on the intermittent runs."""
    correct = sum(r["correct"] == "yes" for r in rows)
    lines = [f"§6.3: runs complete with the correct result = {correct}/{len(rows)}"]
    for a in algorithms:
        runs = [r for r in rows if r["algorithm"] == a and r["status"] == "ok"]
        waits = [float(r["runtime_wait_count"]) for r in runs]
        failures = [float(r["runtime_recovery_boots"]) for r in runs]
        lines.append(
            f"§6.3: {a} waits per run = {min(waits):.0f}--{max(waits):.0f}, "
            f"mean {_mean(waits):.0f}"
        )
        lines.append(
            f"§6.3: {a} power failures per run = at most {max(failures):.0f}, "
            f"mean {_mean(failures):.1f}"
        )
    geomeans = []
    for a in algorithms:
        if a == "milp":
            continue
        per_bench = _speedups(rows, a)
        gm = _gmean([_gmean(ratios) for ratios in per_bench])
        geomeans.append(gm)
        every_run = all(x > 1 for ratios in per_bench for x in ratios)
        lines += [
            f"§6.3: geomean speedup of milp over {a} = {gm:.1f}x",
            f"§6.3: milp faster than {a} on every run = {'yes' if every_run else 'no'}",
        ]
    lines.append(
        f"Intro: speedup of milp over the baselines = "
        f"{min(geomeans):.2f}x--{max(geomeans):.2f}x"
    )
    if decomp:
        share = {
            a: v["share"]["t_recharge"]
            for a, v in _per_algo(decomp, algorithms).items()
        }
        others = [v for a, v in share.items() if a != "milp"]
        lines.append(
            f"§6.3: waiting share of time = milp {100 * share['milp']:.0f}%, "
            f"baselines {100 * min(others):.0f}--{100 * max(others):.0f}%"
        )
    return lines


def summarize(result_dir: Path, benchmarks: list[str], algorithms: list[str]) -> None:
    """Write summary.csv and numbers.txt and, when continuous/ holds the
    continuous-power times, decomposition.csv, decomposition.tex and
    decomposition_per_benchmark.tex into *result_dir*."""
    rows = _read_runs(result_dir, benchmarks, algorithms)
    with open(result_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=SUMMARY_COLUMNS, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)

    decomp = _decompose(result_dir, rows, algorithms)
    (result_dir / "numbers.txt").write_text(
        "\n".join(_numbers(rows, algorithms, decomp)) + "\n"
    )
    if not decomp:
        return
    with open(result_dir / "decomposition.csv", "w", newline="") as f:
        w = csv.DictWriter(
            f, fieldnames=["benchmark", "algorithm", "trace", "t_total"] + COMPONENTS
        )
        w.writeheader()
        w.writerows(decomp)
    (result_dir / "decomposition.tex").write_text(
        _decomposition_table(_per_algo(decomp, algorithms))
    )
    (result_dir / "decomposition_per_benchmark.tex").write_text(
        _per_benchmark_table(decomp, benchmarks, algorithms)
    )
