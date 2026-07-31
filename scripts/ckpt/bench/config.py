"""Benchmark and capacitor discovery/filtering.

Replaces the benchmark-finding and capacitor-config logic duplicated
across run_milp.sh, run_rockclimb.sh, and run_schematic.sh.
"""

from __future__ import annotations

import warnings
from dataclasses import dataclass
from pathlib import Path

from ..env import ProjectEnv
from ..errors import ConfigError

_DEFAULT_CAPS = ["1uF", "5uF", "10uF", "50uF", "100uF"]

_ENERGY_CONFIG_MAP: dict[str, str] = {
    "milp": "assembly_params.json",
    "rockclimb": "assembly_params.json",
    "schematic": "assembly_params.json",
    "schematicO3": "assembly_params.json",
}


def discover_benchmarks(
    env: ProjectEnv,
    filter_names: list[str] | None,
) -> list[Path]:
    """Find benchmark C files in benchmarks/intermittent/.

    If *filter_names* is given, only return matching ones (matched by stem
    without .c extension).  Warns on names that don't match any file.
    If ``None``, returns all ``.c`` files sorted alphabetically.
    """
    bench_dir = env.project_dir / "benchmarks" / "intermittent"

    if filter_names is not None:
        results: list[Path] = []
        for name in filter_names:
            candidate = bench_dir / f"{name}.c"
            if candidate.is_file():
                results.append(candidate)
            else:
                warnings.warn(f"Benchmark not found: {candidate}", stacklevel=2)
        return results

    return sorted(bench_dir.glob("*.c"))


@dataclass(frozen=True)
class CapacitorConfig:
    """A single capacitor size with its config file path."""

    label: str  # e.g., "1uF"
    config_path: Path


def discover_capacitors(
    env: ProjectEnv,
    algorithm: str,
    filter_caps: list[str] | None,
) -> list[CapacitorConfig]:
    """Discover capacitor configurations for an algorithm.

    *algorithm*: ``"milp"``, ``"rockclimb"``, or ``"schematic"``.

    Looks for ``benchmarks/config_{cap}.json`` files.
    Default caps: 1uF, 5uF, 10uF, 50uF, 100uF.

    If *filter_caps* is given, only return matching ones.  Raises
    :class:`SystemExit` if no caps match.
    """
    bench_dir = env.project_dir / "benchmarks"
    caps_to_check = filter_caps if filter_caps else _DEFAULT_CAPS

    results: list[CapacitorConfig] = []
    for cap in caps_to_check:
        cfg = bench_dir / f"config_{cap}.json"
        if cfg.is_file():
            results.append(CapacitorConfig(label=cap, config_path=cfg))
        elif filter_caps is not None:
            # Only warn when the user explicitly asked for this cap
            warnings.warn(f"Capacitor config not found: {cfg}", stacklevel=2)

    if not results:
        available = ", ".join(_DEFAULT_CAPS)
        raise ConfigError(
            f"No matching capacitor sizes for {algorithm}. Available: {available}"
        )

    return results


def default_energy_config(env: ProjectEnv, algorithm: str) -> Path:
    """Return the default energy config path for an algorithm.

    - milp / rockclimb / schematic: ``assembly_params.json``
    """
    filename = _ENERGY_CONFIG_MAP.get(algorithm)
    if filename is None:
        raise ConfigError(
            f"Unknown algorithm: {algorithm!r}. "
            f"Expected one of: {', '.join(_ENERGY_CONFIG_MAP)}"
        )
    return env.project_dir / "benchmarks" / filename


def cap_sort_key(cap: str) -> tuple[float, str]:
    """Sort key ordering capacitor labels numerically (e.g. 5uF < 10uF)."""
    if cap.endswith("uF"):
        try:
            return float(cap.removesuffix("uF")), cap
        except ValueError:
            pass
    return float("inf"), cap
