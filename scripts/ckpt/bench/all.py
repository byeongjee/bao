"""Sequential full benchmark matrix, followed by plotting.

Runs each checkpointing algorithm twice (with and without device debug)
plus the uninstrumented baseline, writing one CSV per step into a single
result directory, then renders the plots with scripts/plot_results.R so
raw data and figures live side by side.
"""

from __future__ import annotations

import logging
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

from ..env import ProjectEnv
from ..errors import CkptError
from ..toolchain import Toolchain

logger = logging.getLogger(__name__)

# Algorithms whose runtime counters need a device-debug build: each runs twice.
DEBUG_VARIANT_ALGORITHMS = ("milp", "schematic", "rockclimb", "schematicO3")
# Steps with no device-debug counters to collect: each runs once.
SINGLE_RUN_ALGORITHMS = ("uninstrumented", "uninstrumentedO0", "chunked")
ALL_ALGORITHMS = DEBUG_VARIANT_ALGORITHMS + SINGLE_RUN_ALGORITHMS
# uninstrumented is included because it is the normalization reference in
# scripts/plot_config.json.
DEFAULT_ALGORITHMS = DEBUG_VARIANT_ALGORITHMS + ("uninstrumented",)

STATUS_OK = "ok"
STATUS_SKIPPED = "skipped"
STATUS_FAILED = "failed"


@dataclass(frozen=True)
class BenchAllOptions:
    """Knobs forwarded to every step that accepts them."""

    benchmarks: list[str] | None
    caps: list[str] | None
    halt_mode: str
    estimator_mode: str
    energy_config: Path | None
    cpu_freq: int
    capture_timeout_seconds: float
    pass_log_level: str


@dataclass(frozen=True)
class BenchStep:
    """One benchmark invocation and the CSV it writes."""

    label: str
    csv_name: str
    algorithm: str
    device_debug: bool


@dataclass(frozen=True)
class StepOutcome:
    """What happened to a single step."""

    label: str
    csv_path: Path
    status: str
    detail: str


def plan_steps(algorithms: list[str]) -> list[BenchStep]:
    """Expand algorithm names into the ordered list of steps to run.

    The CSV names match the spellings plot_results.R prefers:
    ``<algo>_debug.csv`` for device-debug runs, ``<algo>.csv`` otherwise.
    """
    steps: list[BenchStep] = []
    for algorithm in algorithms:
        if algorithm in DEBUG_VARIANT_ALGORITHMS:
            steps.append(
                BenchStep(
                    label=f"{algorithm} (device-debug)",
                    csv_name=f"{algorithm}_debug.csv",
                    algorithm=algorithm,
                    device_debug=True,
                )
            )
            steps.append(
                BenchStep(
                    label=f"{algorithm} (no device-debug)",
                    csv_name=f"{algorithm}.csv",
                    algorithm=algorithm,
                    device_debug=False,
                )
            )
        else:
            steps.append(
                BenchStep(
                    label=algorithm,
                    csv_name=f"{algorithm}.csv",
                    algorithm=algorithm,
                    device_debug=False,
                )
            )
    return steps


def _run_step(
    env: ProjectEnv,
    tc: Toolchain,
    opts: BenchAllOptions,
    *,
    step: BenchStep,
    output_csv: Path,
) -> None:
    """Dispatch one step to its benchmark runner."""
    if step.algorithm == "milp":
        from .milp import run_milp_benchmarks

        run_milp_benchmarks(
            env,
            tc,
            benchmarks=opts.benchmarks,
            caps=opts.caps,
            output_csv=output_csv,
            device_debug=step.device_debug,
            capture_timeout_seconds=opts.capture_timeout_seconds,
            halt_mode=opts.halt_mode,
            estimator_mode=opts.estimator_mode,
            energy_config=opts.energy_config,
            cpu_freq=opts.cpu_freq,
            coarse_allocation=False,
            milp_gap=0.0,
            pass_log_level=opts.pass_log_level,
            accumulate_keys_file=None,
        )
    elif step.algorithm == "rockclimb":
        from .rockclimb import run_rockclimb_benchmarks

        run_rockclimb_benchmarks(
            env,
            tc,
            benchmarks=opts.benchmarks,
            caps=opts.caps,
            output_csv=output_csv,
            device_debug=step.device_debug,
            capture_timeout_seconds=opts.capture_timeout_seconds,
            halt_mode=opts.halt_mode,
            energy_config=opts.energy_config,
            cpu_freq=opts.cpu_freq,
            max_unroll=4,
            pass_log_level=opts.pass_log_level,
            accumulate_keys_file=None,
        )
    elif step.algorithm in ("schematic", "schematicO3"):
        from .schematic import run_schematic_benchmarks

        run_schematic_benchmarks(
            env,
            tc,
            benchmarks=opts.benchmarks,
            caps=opts.caps,
            output_csv=output_csv,
            device_debug=step.device_debug,
            capture_timeout_seconds=opts.capture_timeout_seconds,
            halt_mode=opts.halt_mode,
            energy_config=opts.energy_config,
            trace_config=None,
            estimator_mode=opts.estimator_mode,
            cpu_freq=opts.cpu_freq,
            clang_opt_level=3 if step.algorithm == "schematicO3" else 0,
            pass_log_level=opts.pass_log_level,
            algorithm_label=step.algorithm,
            accumulate_keys_file=None,
            force_checkpoint_on_incompatible_loops=False,
            recompute_energy_after_new_checkpoint=False,
        )
    elif step.algorithm in ("uninstrumented", "uninstrumentedO0"):
        from .uninstrumented import run_uninstrumented_benchmarks

        run_uninstrumented_benchmarks(
            env,
            tc,
            benchmarks=opts.benchmarks,
            output_csv=output_csv,
            capture_timeout_seconds=opts.capture_timeout_seconds,
            cpu_freq=opts.cpu_freq,
            algorithm_label=step.algorithm,
            clang_opt_level=0 if step.algorithm == "uninstrumentedO0" else 3,
            opt_level=3,
        )
    elif step.algorithm == "chunked":
        from .chunked import run_chunked_benchmarks

        run_chunked_benchmarks(
            env,
            tc,
            benchmarks=opts.benchmarks,
            caps=opts.caps,
            output_csv=output_csv,
            energy_config=opts.energy_config,
            capture_timeout_seconds=opts.capture_timeout_seconds,
            cpu_freq=opts.cpu_freq,
            pass_log_level=opts.pass_log_level,
            clang_opt_level=3,
            opt_level=3,
        )
    else:
        raise CkptError(f"Unknown benchmark algorithm: {step.algorithm}")


def run_plot(
    env: ProjectEnv, *, result_dir: Path, plot_config: Path | None
) -> StepOutcome:
    """Render plots from *result_dir* into ``<result_dir>/plots``."""
    plots_dir = result_dir / "plots"
    script = env.project_dir / "scripts" / "plot_results.R"
    rscript = shutil.which("Rscript")
    if rscript is None:
        return StepOutcome(
            label="plot",
            csv_path=plots_dir,
            status=STATUS_FAILED,
            detail="Rscript not found on PATH",
        )

    cmd = [
        rscript,
        str(script),
        "--result-dir",
        str(result_dir),
        "--output-dir",
        str(plots_dir),
    ]
    if plot_config is not None:
        cmd += ["--config", str(plot_config)]

    logger.info("Plotting: %s", " ".join(cmd))
    completed = subprocess.run(cmd, cwd=env.project_dir, check=False)
    if completed.returncode != 0:
        return StepOutcome(
            label="plot",
            csv_path=plots_dir,
            status=STATUS_FAILED,
            detail=f"plot_results.R exited with {completed.returncode}",
        )
    return StepOutcome(label="plot", csv_path=plots_dir, status=STATUS_OK, detail="")


def run_bench_all(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    result_dir: Path,
    algorithms: list[str],
    halt_mode: str,
    estimator_mode: str,
    energy_config: Path | None,
    cpu_freq: int,
    capture_timeout_seconds: float,
    pass_log_level: str,
    skip_existing: bool,
    plot: bool,
    plot_config: Path | None,
) -> list[StepOutcome]:
    """Run every step sequentially, then plot; returns one outcome per step.

    A failing step is logged and does not abort the run: later steps and
    the plotting stage still execute over whatever completed.
    """
    result_dir.mkdir(parents=True, exist_ok=True)
    opts = BenchAllOptions(
        benchmarks=benchmarks,
        caps=caps,
        halt_mode=halt_mode,
        estimator_mode=estimator_mode,
        energy_config=energy_config,
        cpu_freq=cpu_freq,
        capture_timeout_seconds=capture_timeout_seconds,
        pass_log_level=pass_log_level,
    )

    steps = plan_steps(algorithms)
    outcomes: list[StepOutcome] = []
    for index, step in enumerate(steps, start=1):
        csv_path = result_dir / step.csv_name
        if skip_existing and csv_path.exists():
            logger.info(
                "[%d/%d] Skipping %s (%s already exists)",
                index,
                len(steps),
                step.label,
                csv_path,
            )
            outcomes.append(
                StepOutcome(step.label, csv_path, STATUS_SKIPPED, "CSV already exists")
            )
            continue

        logger.info("[%d/%d] === %s -> %s ===", index, len(steps), step.label, csv_path)
        try:
            _run_step(env, tc, opts, step=step, output_csv=csv_path)
        except (CkptError, subprocess.SubprocessError, OSError) as exc:
            logger.error("Step '%s' failed: %s", step.label, exc)
            outcomes.append(StepOutcome(step.label, csv_path, STATUS_FAILED, str(exc)))
            continue
        outcomes.append(StepOutcome(step.label, csv_path, STATUS_OK, ""))

    if plot:
        outcomes.append(run_plot(env, result_dir=result_dir, plot_config=plot_config))

    return outcomes


def format_summary(outcomes: list[StepOutcome], result_dir: Path) -> str:
    """Format the per-step status summary."""
    lines = ["=== Bench All Summary ===", f"Result directory: {result_dir}", ""]
    label_width = max(len("step"), *(len(o.label) for o in outcomes))
    status_width = max(len(STATUS_SKIPPED), len(STATUS_FAILED))
    lines.append(f"{'step':<{label_width}}  {'status':<{status_width}}  output")
    for outcome in outcomes:
        lines.append(
            f"{outcome.label:<{label_width}}  {outcome.status:<{status_width}}  "
            f"{outcome.csv_path}"
        )

    failures = [o for o in outcomes if o.status == STATUS_FAILED]
    if failures:
        lines.append("")
        for outcome in failures:
            lines.append(f"{STATUS_FAILED}: {outcome.label}: {outcome.detail}")
    return "\n".join(lines)


def all_ok(outcomes: list[StepOutcome]) -> bool:
    """True when no step failed."""
    return all(o.status != STATUS_FAILED for o in outcomes)
