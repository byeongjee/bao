"""Three smaller continuous-power experiments of the paper, each run into
its own result directory, which ends with the paper's numbers in
``numbers.txt`` (one ``<paper location>: <what> = <value>`` per line):

- chunking overhead (§6.4): uninstrumented, chunk-only and Bao at 10uF;
- trip-count annotations (§6.5): Bao at 10uF with and without annotations,
  plus a semantic verification of the builds without them;
- constant-alloc (§6.6.2): Bao at 5/10/50uF with per-region and with
  constant allocation.

With ``--save-build`` only the compile steps run and no numbers are written.
"""

from __future__ import annotations

import csv
import logging
import math
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from ..env import ProjectEnv
from ..saved_build import SavedBuild
from ..toolchain import Toolchain

logger = logging.getLogger(__name__)

_CPU_FREQ = 16_000_000

# Columns of a MILP bench row that describe the problem the MILP solves.
# Without annotations, a benchmark whose loop bounds are all inferred
# statically gets the same summarized CFG and so the same problem; its
# solution can still differ where the solver has equally good choices.
_PROBLEM_COLUMNS = [
    "basic_blocks",
    "edges",
    "abstract_cfg_blocks",
    "abstract_cfg_edges",
    "milp_variables",
    "milp_constraints",
]

_TIME = "execution_time_us"
_HITS = "runtime_region_boundary_calls"


@dataclass(frozen=True)
class _Common:
    """Options every step of these experiments shares."""

    benchmarks: list[str] | None
    capture_timeout_seconds: float
    pass_log_level: str
    skip_existing: bool
    saved_build: SavedBuild | None

    @property
    def saving(self) -> bool:
        return self.saved_build is not None and self.saved_build.save


def _step(
    common: _Common, csv_path: Path, run: Callable[[SavedBuild | None], None]
) -> None:
    """Run one step writing *csv_path*, unless it exists and skip_existing."""
    if common.skip_existing and csv_path.exists():
        logger.info("Skipping %s (already exists)", csv_path)
        return
    logger.info("=== %s ===", csv_path)
    saved_build = common.saved_build.sub(csv_path.stem) if common.saved_build else None
    run(saved_build)


def _milp_step(
    env: ProjectEnv,
    tc: Toolchain,
    common: _Common,
    csv_path: Path,
    *,
    caps: list[str],
    device_debug: bool,
    coarse_allocation: bool,
    tripcount_annotations: bool,
) -> None:
    from .milp import run_milp_benchmarks

    _step(
        common,
        csv_path,
        lambda saved_build: run_milp_benchmarks(
            env,
            tc,
            benchmarks=common.benchmarks,
            caps=caps,
            output_csv=csv_path,
            device_debug=device_debug,
            capture_timeout_seconds=common.capture_timeout_seconds,
            halt_mode="swbor",
            estimator_mode="assembly",
            energy_config=None,
            cpu_freq=_CPU_FREQ,
            coarse_allocation=coarse_allocation,
            tripcount_annotations=tripcount_annotations,
            milp_gap=0.0,
            pass_log_level=common.pass_log_level,
            accumulate_keys_file=None,
            extra_defines=[],
            saved_build=saved_build,
        ),
    )


def _load(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    """(benchmark, capacitor) -> row, for rows with status ok. The
    uninstrumented CSV has no capacitor, so its key has ``""``."""
    rows = {}
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


def _ratios[K](
    num: dict[K, dict[str, str]],
    den: dict[K, dict[str, str]],
    keys: list[K],
    column: str,
) -> dict[K, float]:
    """num/den of *column* for each key both have a value for: a bench run
    without a device leaves the runtime columns blank."""
    return {
        k: float(num[k][column]) / float(den[k][column])
        for k in keys
        if k in num and k in den and num[k][column] and den[k][column]
    }


def _write_numbers(result_dir: Path, lines: list[str]) -> None:
    text = "\n".join(lines) + "\n"
    (result_dir / "numbers.txt").write_text(text)
    logger.info("Numbers (%s):\n%s", result_dir / "numbers.txt", text)


# ---------------------------------------------------------------------------
# §6.4 chunking overhead
# ---------------------------------------------------------------------------


def chunking_overhead_numbers(result_dir: Path) -> list[str]:
    """Execution time of chunked.csv and milp.csv relative to
    uninstrumented.csv, at 10uF."""
    base = {b: r for (b, _), r in _load(result_dir / "uninstrumented.csv").items()}
    lines = []
    for name, what in [("chunked", "chunk-only"), ("milp", "full Bao")]:
        rows = {
            b: r
            for (b, cap), r in _load(result_dir / f"{name}.csv").items()
            if cap == "10uF"
        }
        ratios = _ratios(rows, base, list(rows), _TIME)
        if not ratios:
            continue
        worst = max(ratios, key=lambda b: ratios[b])
        lines += [
            (
                f"§6.4: {what} execution time increase over uninstrumented, geomean"
                f" = {100 * (_gmean(list(ratios.values())) - 1):.1f}%"
            ),
            (
                f"§6.4: {what} execution time increase over uninstrumented, maximum"
                f" = {100 * (ratios[worst] - 1):.1f}% ({worst})"
            ),
        ]
    return lines


def run_chunking_overhead(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    result_dir: Path,
    capture_timeout_seconds: float,
    pass_log_level: str,
    skip_existing: bool,
    saved_build: SavedBuild | None,
) -> None:
    """uninstrumented.csv, chunked.csv (10uF) and milp.csv (10uF, no
    device debug), then the §6.4 numbers."""
    from ..compile.chunked import OPT_LEVELS
    from ..compile.uninstrumented import OPT_LEVELS_BY_LABEL
    from .chunked import run_chunked_benchmarks
    from .uninstrumented import run_uninstrumented_benchmarks

    common = _Common(
        benchmarks, capture_timeout_seconds, pass_log_level, skip_existing, saved_build
    )
    clang_opt, opt = OPT_LEVELS_BY_LABEL["uninstrumented"]
    _step(
        common,
        result_dir / "uninstrumented.csv",
        lambda sb: run_uninstrumented_benchmarks(
            env,
            tc,
            benchmarks=benchmarks,
            output_csv=result_dir / "uninstrumented.csv",
            capture_timeout_seconds=capture_timeout_seconds,
            cpu_freq=_CPU_FREQ,
            algorithm_label="uninstrumented",
            clang_opt_level=clang_opt,
            opt_level=opt,
            extra_defines=[],
            saved_build=sb,
        ),
    )
    _step(
        common,
        result_dir / "chunked.csv",
        lambda sb: run_chunked_benchmarks(
            env,
            tc,
            benchmarks=benchmarks,
            caps=["10uF"],
            output_csv=result_dir / "chunked.csv",
            energy_config=None,
            capture_timeout_seconds=capture_timeout_seconds,
            cpu_freq=_CPU_FREQ,
            pass_log_level=pass_log_level,
            clang_opt_level=OPT_LEVELS[0],
            opt_level=OPT_LEVELS[1],
            saved_build=sb,
        ),
    )
    _milp_step(
        env,
        tc,
        common,
        result_dir / "milp.csv",
        caps=["10uF"],
        device_debug=False,
        coarse_allocation=False,
        tripcount_annotations=True,
    )
    if not common.saving:
        _write_numbers(result_dir, chunking_overhead_numbers(result_dir))


# ---------------------------------------------------------------------------
# §6.5 trip-count annotations
# ---------------------------------------------------------------------------


def trip_count_numbers(result_dir: Path, verify_report: str | None) -> list[str]:
    """Compare milp[_debug].csv (annotations) with
    milp_no_tripcount[_debug].csv at 10uF."""

    def load(name: str) -> dict[str, dict[str, str]]:
        return {
            b: r
            for (b, cap), r in _load(result_dir / f"{name}.csv").items()
            if cap == "10uF"
        }

    with_ann, with_ann_debug = load("milp"), load("milp_debug")
    without, without_debug = load("milp_no_tripcount"), load("milp_no_tripcount_debug")
    changed = [
        b
        for b in with_ann
        if b in without
        and any(with_ann[b][c] != without[b][c] for c in _PROBLEM_COLUMNS)
    ]
    time = _ratios(without, with_ann, changed, _TIME)
    hits = _ratios(without_debug, with_ann_debug, changed, _HITS)
    lines = [
        (
            f"§6.5: benchmarks compiled without annotations = {len(without)} of"
            f" {len(with_ann)}"
        ),
    ]
    if verify_report is not None:
        lines.append(f"§6.5: verification without annotations = {verify_report}")
    lines += [
        (
            f"§6.5: benchmarks with the same MILP problem as with annotations"
            f" = {len(with_ann) - len(changed)} of {len(with_ann)}"
        ),
    ]
    if time:
        worst = max(time, key=lambda b: time[b])
        lines += [
            (
                f"§6.5: execution time increase on the other benchmarks, geomean"
                f" = {_gmean(list(time.values())):.2f}x"
            ),
            (
                f"§6.5: execution time increase on the other benchmarks, maximum"
                f" = {time[worst]:.2f}x ({worst})"
            ),
        ]
    if hits:
        lines.append(
            f"§6.5: region boundary hit increase on the other benchmarks, geomean"
            f" = {_gmean(list(hits.values())):.0f}x"
        )
    return lines


def run_trip_count(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    result_dir: Path,
    capture_timeout_seconds: float,
    pass_log_level: str,
    skip_existing: bool,
    saved_build: SavedBuild | None,
) -> None:
    """Bao at 10uF with and without trip-count annotations, each with and
    without device debug, and ``verify milp --no-tripcount`` at 10uF into
    verify.txt; then the §6.5 numbers."""
    from ..verify.all import format_report
    from ..verify.common import Status
    from ..verify.milp import verify_milp

    common = _Common(
        benchmarks, capture_timeout_seconds, pass_log_level, skip_existing, saved_build
    )
    for tripcount, suffix in [(True, ""), (False, "_no_tripcount")]:
        for device_debug in (True, False):
            name = f"milp{suffix}{'_debug' if device_debug else ''}"
            _milp_step(
                env,
                tc,
                common,
                result_dir / f"{name}.csv",
                caps=["10uF"],
                device_debug=device_debug,
                coarse_allocation=False,
                tripcount_annotations=tripcount,
            )

    verify_txt = result_dir / "verify.txt"
    if common.skip_existing and verify_txt.exists():
        logger.info("Skipping %s (already exists)", verify_txt)
    else:
        halt_mode = "bor"
        results = verify_milp(
            env,
            tc,
            benchmarks=benchmarks,
            caps=["10uF"],
            halt_mode=halt_mode,
            energy_config=None,
            estimator_mode="assembly",
            cpu_freq=_CPU_FREQ,
            capture_timeout_seconds=capture_timeout_seconds,
            coarse_allocation=False,
            tripcount_annotations=False,
            pass_log_level=pass_log_level,
            saved_build=saved_build.sub("verify") if saved_build else None,
        )
        if not common.saving:
            passed = sum(1 for r in results if r.status is Status.PASS)
            report = format_report({"milp": results}, halt_mode)
            verify_txt.write_text(
                report + f"\n\nsummary: {passed} of {len(results)} passed\n"
            )

    if common.saving:
        return
    _write_numbers(
        result_dir, trip_count_numbers(result_dir, _verify_summary(verify_txt))
    )


def _verify_summary(verify_txt: Path) -> str | None:
    """The ``summary:`` line run_trip_count appends to verify.txt."""
    if not verify_txt.is_file():
        return None
    for line in verify_txt.read_text().splitlines():
        if line.startswith("summary: "):
            return line.removeprefix("summary: ")
    return None


# ---------------------------------------------------------------------------
# §6.6.2 constant-alloc
# ---------------------------------------------------------------------------

_COARSE_CAPS = ["5uF", "10uF", "50uF"]

# The columns that show a changed boundary placement or memory allocation.
# The MILP size columns differ between the two modes by construction.
_COARSE_DECISION_COLUMNS = ("region_boundaries", "vm_placed_global_names")


def milp_coarse_numbers(result_dir: Path) -> list[str]:
    """Compare regional.csv with coarse.csv: solve time over every pair,
    execution time over the pairs whose solution changed."""
    from ..analysis.milp_coarse import (
        summarize_milp_coarse_allocation,
        write_milp_coarse_summary_csv,
    )

    summary = summarize_milp_coarse_allocation(
        result_dir / "regional.csv", result_dir / "coarse.csv"
    )
    write_milp_coarse_summary_csv(summary, result_dir / "summary.csv")
    lines = [
        f"§6.6.2: constant-alloc geomean solve time reduction, {row['capacitor']}"
        f" = {float(row['solve_time_geomean_reduction_pct']):.1f}%"
        for row in summary
        if row["capacitor"] != "ALL"
    ]

    regional = _load(result_dir / "regional.csv")
    coarse = _load(result_dir / "coarse.csv")
    changed = [
        k
        for k in regional
        if k in coarse
        and any(regional[k][c] != coarse[k][c] for c in _COARSE_DECISION_COLUMNS)
    ]
    lines.append(
        "§6.6.2: pairs where constant-alloc changed the boundary placement"
        f" or the memory allocation = {len(changed)}"
    )
    slowdown = _ratios(coarse, regional, changed, _TIME)
    if slowdown:
        worst = max(slowdown, key=lambda k: slowdown[k])
        lines.append(
            "§6.6.2: maximum constant-alloc slowdown on those pairs"
            f" = {100 * (slowdown[worst] - 1):.1f}% ({worst[0]} at {worst[1]})"
        )
    return lines


def run_milp_coarse(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    result_dir: Path,
    capture_timeout_seconds: float,
    pass_log_level: str,
    skip_existing: bool,
    saved_build: SavedBuild | None,
) -> None:
    """Bao at 5/10/50uF with per-region (regional.csv) and constant
    (coarse.csv) allocation, without device debug; then summary.csv and the
    §6.6.2 numbers."""
    common = _Common(
        benchmarks, capture_timeout_seconds, pass_log_level, skip_existing, saved_build
    )
    for name, coarse_allocation in [("regional", False), ("coarse", True)]:
        _milp_step(
            env,
            tc,
            common,
            result_dir / f"{name}.csv",
            caps=_COARSE_CAPS,
            device_debug=False,
            coarse_allocation=coarse_allocation,
            tripcount_annotations=True,
        )
    if not common.saving:
        _write_numbers(result_dir, milp_coarse_numbers(result_dir))
