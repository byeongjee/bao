"""Sequential semantic verification of all checkpoint algorithms.

Runs milp, rockclimb, schematic, and schematicO3 through the shared
verify loop (one baseline run per benchmark, shared by every capacitor
and algorithm) and builds a combined status-matrix report.
"""

from __future__ import annotations

from pathlib import Path

from ..env import ProjectEnv
from ..toolchain import Toolchain
from .common import BenchResult, Status, verify_algorithms
from .milp import milp_spec
from .rockclimb import rockclimb_spec
from .schematic import schematic_spec


def verify_all(
    env: ProjectEnv,
    tc: Toolchain,
    *,
    benchmarks: list[str] | None,
    caps: list[str] | None,
    halt_mode: str,
    energy_config: Path | None,
    estimator_mode: str,
    cpu_freq: int,
    capture_timeout_seconds: float,
    pass_log_level: str,
) -> dict[str, list[BenchResult]]:
    """Verify every algorithm; returns per-algorithm result lists."""
    specs = [
        milp_spec(
            env,
            energy_config=energy_config,
            estimator_mode=estimator_mode,
            coarse_allocation=False,
            pass_log_level=pass_log_level,
        ),
        rockclimb_spec(
            env,
            energy_config=energy_config,
            pass_log_level=pass_log_level,
        ),
    ]
    for algorithm_label, clang_opt_level in (("schematic", 0), ("schematicO3", 3)):
        specs.append(
            schematic_spec(
                env,
                energy_config=energy_config,
                estimator_mode=estimator_mode,
                clang_opt_level=clang_opt_level,
                pass_log_level=pass_log_level,
                algorithm_label=algorithm_label,
                force_checkpoint_on_incompatible_loops=False,
                recompute_energy_after_new_checkpoint=False,
            )
        )

    return verify_algorithms(
        env,
        tc,
        algorithms=specs,
        benchmarks=benchmarks,
        caps=caps,
        halt_mode=halt_mode,
        cpu_freq=cpu_freq,
        capture_timeout_seconds=capture_timeout_seconds,
    )


def format_report(
    results_by_algorithm: dict[str, list[BenchResult]],
    halt_mode: str,
) -> str:
    """Format the combined verification report as a status matrix."""
    algorithms = list(results_by_algorithm)

    # (benchmark, cap) -> algorithm -> result, preserving encounter order.
    matrix: dict[tuple[str, str], dict[str, BenchResult]] = {}
    for algorithm, results in results_by_algorithm.items():
        for r in results:
            matrix.setdefault((r.name, r.cap_label), {})[algorithm] = r

    pairs = list(matrix)
    name_width = max(len("benchmark"), *(len(name) for name, _ in pairs))
    cap_width = max(len("cap"), *(len(cap) for _, cap in pairs))
    col_widths = [max(len(a), len(Status.ERROR.value)) for a in algorithms]

    lines: list[str] = []
    lines.append("=== Verify All Report ===")
    cap_labels = ", ".join(dict.fromkeys(cap for _, cap in pairs))
    lines.append(f"Halt mode: {halt_mode} | Capacitors: {cap_labels}")
    lines.append("")

    header_cells = [f"{a:<{w}}" for a, w in zip(algorithms, col_widths)]
    lines.append(
        f"{'benchmark':<{name_width}}  {'cap':<{cap_width}}  " + "  ".join(header_cells)
    )
    for name, cap in pairs:
        cells = []
        for algorithm, width in zip(algorithms, col_widths):
            r = matrix[(name, cap)].get(algorithm)
            cells.append(f"{r.status.value if r else '-':<{width}}")
        lines.append(f"{name:<{name_width}}  {cap:<{cap_width}}  " + "  ".join(cells))
    lines.append("")

    details = [
        (algorithm, r)
        for algorithm in algorithms
        for r in results_by_algorithm[algorithm]
        if r.status is not Status.PASS and r.detail
    ]
    if details:
        for algorithm, r in details:
            lines.append(
                f"{r.status.value}: {algorithm} {r.name} [{r.cap_label}]: {r.detail}"
            )
        lines.append("")

    for algorithm in algorithms:
        results = results_by_algorithm[algorithm]
        counts = {s: sum(1 for r in results if r.status is s) for s in Status}
        lines.append(
            f"{algorithm}: {counts[Status.PASS]}/{len(results)} PASSED, "
            f"{counts[Status.FAIL]} FAILED, {counts[Status.SKIP]} SKIPPED, "
            f"{counts[Status.ERROR]} ERRORS"
        )

    return "\n".join(lines)
