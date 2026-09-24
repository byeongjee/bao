"""The full intermittent-power experiment, and the continuous-power times its
decomposition needs.

``run_intermittent_all`` measures each benchmark's expected result
(baseline.csv), runs every algorithm on every trace (``<bench>/<algo>.csv``),
merges the runs into summary.csv (plus the decomposition tables when
continuous/ is present) and plots them with scripts/plot_intermittent.R.

``run_decomposition`` (``ckpt bench decomposition``) writes continuous/: the
same wait-mode binaries and the uninstrumented baseline under constant power.
It needs the Otii wired directly to the board rail (docs/intermittent.md), so
it is a separate command.
"""

from __future__ import annotations

import csv
import logging
import shutil
import subprocess
from contextlib import closing
from pathlib import Path

from ..bench.all import BenchAllOptions, BenchStep, run_step
from ..device.otii import debugger_connection
from ..device.saleae import discover_saleae
from ..env import ProjectEnv
from ..errors import CkptError
from ..saved_build import SAVED_ERRORS, SavedBuild, build_workdir
from ..toolchain import Toolchain
from ..verify.common import _load_or_compile_baseline, _run_baseline
from .runner import _SHRINK_DEFINES, run_intermittent_benchmarks
from .summary import summarize

logger = logging.getLogger(__name__)

# In the order of the paper's figure and tables.
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
ALGORITHMS = ["milp", "rockclimb", "schematic", "schematicO3"]
TRACES = [str(i) for i in range(1, 11)]

_CAP = "board"
_CPU_FREQ = 16_000_000

# A step that fails with one of these is logged and skipped, as in bench all.
_STEP_ERRORS = (CkptError, subprocess.SubprocessError, OSError)


def _sub(saved_build: SavedBuild | None, *names: str) -> SavedBuild | None:
    if saved_build is None:
        return None
    for name in names:
        saved_build = saved_build.sub(name)
    return saved_build


def _measure_baselines(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str],
    output_csv: Path,
    capture_timeout_seconds: float,
    saved_build: SavedBuild | None,
) -> None:
    """Run each benchmark's uninstrumented build under continuous power and
    write its result, the expected result of the intermittent runs."""
    bench_dir = env.project_dir / "benchmarks" / "intermittent"
    if saved_build is not None and saved_build.save:
        for b in benchmarks:
            sb = saved_build.sub(b)
            with build_workdir(sb, prefix="") as workdir:
                try:
                    _load_or_compile_baseline(
                        tc, env, sb, bench_dir / f"{b}.c", workdir, _CPU_FREQ
                    )
                except SAVED_ERRORS as exc:
                    logger.error("%s baseline compilation failed: %s", b, exc)
        return

    rows = []
    with debugger_connection() as otii, closing(discover_saleae()) as saleae:
        for b in benchmarks:
            sb = _sub(saved_build, b)
            with build_workdir(sb, prefix=f"baseline_{b}_") as workdir:
                result, err = _run_baseline(
                    tc,
                    env,
                    saved_build=sb,
                    bench_path=bench_dir / f"{b}.c",
                    workdir=workdir,
                    cpu_freq=_CPU_FREQ,
                    saleae_manager=saleae,
                    otii=otii,
                    capture_timeout_seconds=capture_timeout_seconds,
                )
            logger.info("%s baseline result=%s err=%s", b, result, err)
            rows.append({"benchmark": b, "result": result or "", "error": err or ""})
    with open(output_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["benchmark", "result", "error"])
        w.writeheader()
        w.writerows(rows)


def _row_count(path: Path) -> int:
    with open(path, newline="") as f:
        return sum(1 for _ in csv.DictReader(f))


def _plot(env: ProjectEnv, result_dir: Path) -> None:
    rscript = shutil.which("Rscript")
    if rscript is None:
        logger.warning("Rscript not found on PATH; skipping plots")
        return
    cmd = [
        rscript,
        str(env.project_dir / "scripts" / "plot_intermittent.R"),
        "--result-dir",
        str(result_dir),
        "--output-dir",
        str(result_dir),
    ]
    logger.info("Plotting: %s", " ".join(cmd))
    if subprocess.run(cmd, cwd=env.project_dir, check=False).returncode != 0:
        logger.error("plot_intermittent.R failed")


def run_intermittent_all(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str],
    trace_specs: list[str],
    result_dir: Path,
    max_unroll: int,
    capture_timeout_seconds: float,
    pass_log_level: str,
    skip_existing: bool,
    plot: bool,
    saved_build: SavedBuild | None,
) -> None:
    """Measure the baselines, run every (benchmark, algorithm) on every
    trace, then summarize and plot.

    With *skip_existing*, baseline.csv is kept if present and a
    ``<bench>/<algo>.csv`` with a row per trace is not run again.
    With ``--save-build`` only the compile steps run.
    """
    saving = saved_build is not None and saved_build.save
    if not saving:
        result_dir.mkdir(parents=True, exist_ok=True)

    baseline_csv = result_dir / "baseline.csv"
    if skip_existing and baseline_csv.is_file():
        logger.info("Skipping baselines (%s already exists)", baseline_csv)
    else:
        _measure_baselines(
            env,
            tc,
            benchmarks=benchmarks,
            output_csv=baseline_csv,
            capture_timeout_seconds=capture_timeout_seconds,
            saved_build=saved_build,
        )

    for b in benchmarks:
        for a in ALGORITHMS:
            csv_path = result_dir / b / f"{a}.csv"
            if (
                skip_existing
                and csv_path.is_file()
                and _row_count(csv_path) == len(trace_specs)
            ):
                logger.info("Skipping %s %s (%s is complete)", b, a, csv_path)
                continue
            logger.info("=== %s %s -> %s ===", b, a, csv_path)
            try:
                run_intermittent_benchmarks(
                    env,
                    tc,
                    algorithm=a,
                    benchmarks=[b],
                    caps=[_CAP],
                    trace_specs=trace_specs,
                    output_csv=csv_path,
                    device_debug=False,
                    estimator_mode="assembly",
                    cpu_freq=_CPU_FREQ,
                    max_unroll=max_unroll if a == "rockclimb" else None,
                    pass_log_level=pass_log_level,
                    saved_build=_sub(saved_build, b, a),
                )
            except _STEP_ERRORS as exc:
                logger.error("%s %s failed: %s", b, a, exc)

    if saving:
        return
    summarize(result_dir, benchmarks, ALGORITHMS)
    if plot:
        _plot(env, result_dir)


def run_decomposition(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str],
    result_dir: Path,
    max_unroll: int,
    capture_timeout_seconds: float,
    pass_log_level: str,
    skip_existing: bool,
    saved_build: SavedBuild | None,
) -> None:
    """Write ``continuous/<algo>.csv`` and ``continuous/uninstrumented.csv``:
    the intermittent builds (wait mode, board capacitor, INTERMITTENT_BUILD)
    run under continuous power by ``ckpt bench``. With ``--save-build`` only
    the compile steps run."""
    out_dir = result_dir / "continuous"
    if saved_build is None or not saved_build.save:
        out_dir.mkdir(parents=True, exist_ok=True)
    opts = BenchAllOptions(
        benchmarks=benchmarks,
        caps=[_CAP],
        halt_mode="wait",
        estimator_mode="assembly",
        energy_config=None,
        cpu_freq=_CPU_FREQ,
        coarse_allocation=False,
        milp_gap=0.0,
        max_unroll=max_unroll,
        capture_timeout_seconds=capture_timeout_seconds,
        pass_log_level=pass_log_level,
        extra_defines=list(_SHRINK_DEFINES),
    )
    for algorithm in [*ALGORITHMS, "uninstrumented"]:
        csv_path = out_dir / f"{algorithm}.csv"
        if skip_existing and csv_path.is_file():
            logger.info("Skipping %s (%s already exists)", algorithm, csv_path)
            continue
        logger.info("=== %s -> %s ===", algorithm, csv_path)
        try:
            run_step(
                env,
                tc,
                opts,
                step=BenchStep(
                    label=algorithm,
                    csv_name=csv_path.name,
                    algorithm=algorithm,
                    # bench's default, with which the paper's data was measured.
                    device_debug=True,
                ),
                output_csv=csv_path,
                saved_build=_sub(saved_build, algorithm),
            )
        except _STEP_ERRORS as exc:
            logger.error("%s failed: %s", algorithm, exc)
