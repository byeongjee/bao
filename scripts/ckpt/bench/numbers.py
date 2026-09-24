"""The paper's continuous-power numbers (§6.2, §6.6, abstract, App. E),
computed from the CSVs of a ``bench all`` result directory.

Writes ``numbers.txt`` (one ``<paper location>: <what> = <value>`` per line)
and the MILP compilation statistics tables ``milp_compile_stats.tex`` (§6.6,
10uF) and ``milp_compile_stats_appendix.tex`` (App. E, all capacitors).
A number whose CSVs are missing from the directory is left out.
"""

from __future__ import annotations

import csv
import math
from pathlib import Path

CAPS = ["5uF", "10uF", "50uF"]
BASELINES = ["rockclimb", "schematic", "schematicO3"]

# The paper's short names; the rest appear as in the file names.
_TABLE_LABELS = {"activity_recognition": "ar"}

_TIME = "execution_time_us"
_HITS = "runtime_region_boundary_calls"

type Rows = dict[tuple[str, str], dict[str, str]]


def _load(path: Path) -> Rows | None:
    """(benchmark, capacitor) -> row, for rows with status ok. The
    uninstrumented CSVs have no capacitor, so their key has ``""``."""
    if not path.is_file():
        return None
    rows: Rows = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r["status"] != "ok":
                continue
            cap = r.get("capacitor", "")
            bench = r["benchmark"].removesuffix(f"-{cap}") if cap else r["benchmark"]
            rows[(bench, cap)] = r
    return rows


def _gmean(xs: list[float]) -> float:
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def _ratio_gmean(
    num: Rows | None, den: Rows | None, cap: str, column: str
) -> float | None:
    """Geomean over benchmarks of num/den at *cap*; None if no benchmark has
    both."""
    if num is None or den is None:
        return None
    ratios = [
        float(num[k][column]) / float(den[k][column])
        for k in den
        if k[1] == cap and k in num
    ]
    return _gmean(ratios) if ratios else None


def _value(rows: Rows, bench: str, cap: str, column: str) -> float:
    return float(rows[(bench, cap)][column])


def _times(result_dir: Path) -> list[str]:
    """§6.2 execution-time and region-boundary-hit comparisons."""
    lines: list[str] = []
    milp = _load(result_dir / "milp.csv")
    milp_debug = _load(result_dir / "milp_debug.csv")
    timing = {a: _load(result_dir / f"{a}.csv") for a in BASELINES}
    debug = {a: _load(result_dir / f"{a}_debug.csv") for a in BASELINES}

    for cap in CAPS:
        for a in BASELINES:
            r = _ratio_gmean(timing[a], milp, cap, _TIME)
            if r is not None:
                lines.append(
                    f"§6.2: {a} execution time relative to Bao, "
                    f"geomean, {cap} = {r:.2f}x"
                )

    r = _ratio_gmean(debug["schematic"], milp_debug, "10uF", _HITS)
    if r is not None:
        lines.append(
            "§6.2: schematic region boundary hits relative to Bao, "
            f"geomean, 10uF = {r:.2f}x"
        )

    o3 = _load(result_dir / "uninstrumented.csv")
    o0 = _load(result_dir / "uninstrumentedO0.csv")
    r = _ratio_gmean(o0, o3, "", _TIME)
    if r is not None:
        lines.append(
            f"§6.2: uninstrumented O0 execution time relative to O3, geomean = {r:.2f}x"
        )

    # "Bao reduces X by N%" is 1 - 1/(geomean of baseline/Bao).
    for a, where in [("rockclimb", "§6.2"), ("schematicO3", "§6.2, abstract, §1")]:
        r = _ratio_gmean(timing[a], milp, "10uF", _TIME)
        if r is not None:
            lines.append(
                f"{where}: Bao execution time reduction vs {a}, 10uF "
                f"= {100 * (1 - 1 / r):.1f}%"
            )
        r = _ratio_gmean(debug[a], milp_debug, "10uF", _HITS)
        if r is not None:
            lines.append(
                f"{where}: Bao region boundary hit reduction vs {a}, 10uF "
                f"= {100 * (1 - 1 / r):.1f}%"
            )

    unroll = _load(result_dir / "rockclimb_crc_unroll64.csv")
    if (
        unroll is not None
        and timing["rockclimb"] is not None
        and ("crc", "10uF") in timing["rockclimb"]
    ):
        t = _value(unroll, "crc", "10uF", _TIME)
        base = _value(timing["rockclimb"], "crc", "10uF", _TIME)
        lines.append(
            "§6.2: rockclimb crc 10uF execution time decrease with unroll 64 "
            f"= {100 * (1 - t / base):.1f}%"
        )
        if (
            timing["schematicO3"] is not None
            and ("crc", "10uF") in timing["schematicO3"]
        ):
            s = _value(timing["schematicO3"], "crc", "10uF", _TIME)
            lines.append(
                "§6.2: rockclimb crc 10uF unroll 64 execution time relative to "
                f"schematicO3 = {t / s:.2f}x"
            )
        if milp is not None and ("crc", "10uF") in milp:
            m = _value(milp, "crc", "10uF", _TIME)
            lines.append(
                "§6.2: rockclimb crc 10uF unroll 64 slower than Bao by "
                f"{100 * (t / m - 1):.1f}%"
            )

    sf = ("sensor_fusion", "10uF")
    if (
        debug["schematic"] is not None
        and debug["schematicO3"] is not None
        and sf in debug["schematic"]
        and sf in debug["schematicO3"]
    ):
        r = _value(debug["schematicO3"], "sensor_fusion", "10uF", _HITS) / _value(
            debug["schematic"], "sensor_fusion", "10uF", _HITS
        )
        lines.append(
            "§6.2: sensor_fusion schematicO3 region boundary hits relative to "
            f"schematic, 10uF = {r:.2f}x"
        )
    qs = ("qsort", "10uF")
    if (
        milp is not None
        and timing["schematicO3"] is not None
        and qs in milp
        and qs in timing["schematicO3"]
    ):
        r = _value(milp, "qsort", "10uF", _TIME) / _value(
            timing["schematicO3"], "qsort", "10uF", _TIME
        )
        lines.append(
            "§6.2: qsort Bao execution time reduction vs schematicO3, 10uF "
            f"= {100 * (1 - r):.1f}%"
        )
    return lines


def _solve_times(milp: Rows) -> list[str]:
    """§6.6 MILP solve times."""
    lines: list[str] = []

    def stats(cap: str, exclude: str | None) -> tuple[float, float]:
        ms = [
            float(r["milp_solve_time_ms"])
            for (b, c), r in milp.items()
            if c == cap and b != exclude
        ]
        return max(ms), sum(ms) / len(ms)

    for cap, exclude in [("10uF", None), ("50uF", None), ("5uF", "sensor_fusion")]:
        if not any(c == cap and b != exclude for b, c in milp):
            continue
        worst, mean = stats(cap, exclude)
        but = f" excluding {exclude}" if exclude else ""
        lines.append(f"§6.6: MILP solve time max, {cap}{but} = {worst / 1000:.3f} s")
        lines.append(f"§6.6: MILP solve time mean, {cap}{but} = {mean:.1f} ms")
    sf = milp.get(("sensor_fusion", "5uF"))
    if sf is not None:
        lines.append(f"§6.6: sensor_fusion 5uF MILP variables = {sf['milp_variables']}")
        lines.append(
            "§6.6: sensor_fusion 5uF MILP solve time = "
            f"{float(sf['milp_solve_time_ms']) / 1000:.3f} s"
        )
    return lines


def _stats_rows(milp: Rows, cap: str) -> list[str]:
    out = []
    for b in sorted(b for (b, c) in milp if c == cap):
        r = milp[(b, cap)]
        cells = [
            r["abstract_cfg_blocks"],
            r["abstract_cfg_edges"],
            r["milp_variables"],
            r["milp_constraints"],
            f"{float(r['milp_solve_time_ms']) / 1000:.3f}",
        ]
        name = _TABLE_LABELS.get(b, b).replace("_", r"\_").ljust(15)
        out.append(name + "& " + " & ".join(rf"\num{{{c}}}" for c in cells) + r" \\")
    return out


def _stats_table(body: list[str]) -> str:
    return (
        "\n".join(
            [
                r"\begin{tabular}{lrrrrr}",
                r"\toprule",
                r"Bench. & Blocks & Edges & Vars. & Constr. & Sol. Time (s) \\",
                r"\midrule",
                *body,
                r"\bottomrule",
                r"\end{tabular}",
            ]
        )
        + "\n"
    )


def write_numbers(result_dir: Path) -> None:
    lines = _times(result_dir)
    milp = _load(result_dir / "milp.csv")
    if milp is not None:
        lines += _solve_times(milp)
        (result_dir / "milp_compile_stats.tex").write_text(
            _stats_table(_stats_rows(milp, "10uF"))
        )
        body: list[str] = []
        for i, cap in enumerate(CAPS):
            if i:
                body.append(r"\midrule")
            body.append(
                r"\multicolumn{1}{l}{\textit{"
                + cap.removesuffix("uF")
                + r"\,\textmu{}F}} \\"
            )
            body += _stats_rows(milp, cap)
        (result_dir / "milp_compile_stats_appendix.tex").write_text(_stats_table(body))
    (result_dir / "numbers.txt").write_text("\n".join(lines) + "\n")
