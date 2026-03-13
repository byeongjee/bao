"""Structured output parsing — replaces extract_stat() + grep/awk/sed chains."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass
class PassStatistics:
    """Parsed pass statistics from compiler output.

    Fields return None when the metric was not found in the output,
    rather than defaulting to 0 (so callers can distinguish missing from zero).
    """

    basic_blocks: int | None = None
    edges: int | None = None
    regions: int | None = None
    candidate_globals: int | None = None
    milp_variables: int | None = None
    milp_constraints: int | None = None
    optimal_solution: str | None = None
    region_boundaries: int | None = None
    distributed_checkpoints: int | None = None
    solve_time_ms: int | None = None
    compilation_time_ms: int | None = None
    peak_rss_kb: int | None = None
    profiling_time_ms: int | None = None
    execution_time_ms: int | None = None
    boundary_checks: int | None = None
    # SCHEMATIC-specific
    enabled_checkpoints: int | None = None
    loop_decisions: int | None = None
    paths_analyzed: int | None = None
    runtime_calls_inserted: int | None = None


# Maps stat field name -> list of label strings to search for (first match wins)
_STAT_LABELS: dict[str, list[str]] = {
    "basic_blocks": ["Basic blocks (concrete)", "Basic blocks"],
    "edges": ["Edges (concrete)", "Edges"],
    "regions": ["Regions"],
    "candidate_globals": ["Candidate globals (V_elig)"],
    "milp_variables": ["MILP variables"],
    "milp_constraints": ["MILP constraints"],
    "optimal_solution": ["Optimal solution"],
    "region_boundaries": ["Region boundaries"],
    "distributed_checkpoints": ["Distributed checkpoints inserted", "Boundary commits enabled"],
    "solve_time_ms": ["Solve time (ms)"],
    "compilation_time_ms": ["Compilation time (ms)"],
    "peak_rss_kb": ["Peak RSS (KB)"],
    "profiling_time_ms": ["Profiling time (ms)"],
    "execution_time_ms": ["Execution time (ms)"],
    "boundary_checks": ["Boundary checks"],
    "enabled_checkpoints": ["Enabled checkpoints"],
    "loop_decisions": ["Loop decisions"],
    "paths_analyzed": ["Paths analyzed"],
    "runtime_calls_inserted": ["Runtime calls inserted"],
}


def extract_stat(text: str, *labels: str) -> str | None:
    """Extract the first token after 'label:' for the first matching label.

    This is the Python equivalent of the bash extract_stat() function.
    Returns None if no label matches.
    """
    for label in labels:
        for line in text.splitlines():
            if label + ":" in line:
                after_colon = line.split(label + ":", 1)[1]
                token = after_colon.strip().split()[0] if after_colon.strip() else None
                if token:
                    return token
    return None


def _parse_int(val: str | None) -> int | None:
    if val is None:
        return None
    try:
        return int(val)
    except ValueError:
        # Try float -> int for values like "123.0"
        try:
            return int(float(val))
        except ValueError:
            return None


def parse_pass_output(text: str) -> PassStatistics:
    """Parse pass output text into a PassStatistics dataclass."""
    stats = PassStatistics()
    for field_name, labels in _STAT_LABELS.items():
        raw = extract_stat(text, *labels)
        if field_name == "optimal_solution":
            setattr(stats, field_name, raw)
        else:
            setattr(stats, field_name, _parse_int(raw))
    return stats


@dataclass
class NvmCounters:
    """Runtime counters read from NVM."""

    result: int | None = None
    region_boundary: int | None = None
    save_vreg: int | None = None
    restore_vreg: int | None = None
    store_mem: int | None = None
    restore_mem: int | None = None
    save_reg: int | None = None
    restore_reg: int | None = None
    done: int | None = None


# Maps NVM key=value names -> NvmCounters field names
_NVM_KEY_MAP: dict[str, str] = {
    "__nvm_result": "result",
    "__nvm_done": "done",
    "cnt_boundary": "region_boundary",
    "cnt_save_vreg": "save_vreg",
    "cnt_restore_vreg": "restore_vreg",
    "cnt_store_mem": "store_mem",
    "cnt_restore_mem": "restore_mem",
    "cnt_save_reg": "save_reg",
    "cnt_restore_reg": "restore_reg",
}


def parse_nvm_output(text: str) -> NvmCounters:
    """Parse key=value NVM output into NvmCounters.

    Replaces the sed-based conversion in run_milp.sh/run_rockclimb.sh.
    """
    counters = NvmCounters()
    for line in text.splitlines():
        line = line.strip()
        if "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        field_name = _NVM_KEY_MAP.get(key)
        if field_name is not None:
            setattr(counters, field_name, _parse_int(value.strip()))
    return counters


def nvm_counters_to_labels(counters: NvmCounters) -> str:
    """Convert NvmCounters to label: value format for extract_stat compatibility.

    This replaces the sed conversion in the benchmark runners that converts
    key=value NVM output to label: value format.
    """
    lines = []
    if counters.result is not None:
        lines.append(f"RESULT: {counters.result}")
    if counters.region_boundary is not None:
        lines.append(f"__region_boundary: {counters.region_boundary}")
    if counters.save_vreg is not None:
        lines.append(f"vreg_saves: {counters.save_vreg}")
    if counters.restore_vreg is not None:
        lines.append(f"vreg_restores: {counters.restore_vreg}")
    if counters.store_mem is not None:
        lines.append(f"mem_stores: {counters.store_mem}")
    if counters.restore_mem is not None:
        lines.append(f"mem_restores: {counters.restore_mem}")
    if counters.save_reg is not None:
        lines.append(f"reg_saves: {counters.save_reg}")
    if counters.restore_reg is not None:
        lines.append(f"reg_restores: {counters.restore_reg}")
    return "\n".join(lines)


_INFEASIBLE_PATTERNS = [
    ("blocks exceed energy capacity", "blocks exceed energy capacity"),
    ("Optimization failed", "solver found no feasible solution"),
    ("Region partitioning failed", "region partitioning failed"),
    ("blocks exceed E_safe", "blocks exceed E_safe"),
    ("SCHEMATIC infeasible", "energy capacity too small"),
]


def detect_infeasibility(text: str) -> str | None:
    """Check output for infeasibility indicators.

    Returns a human-readable reason string, or None if feasible.
    """
    for pattern, reason in _INFEASIBLE_PATTERNS:
        if pattern in text:
            return reason
    return None


def has_pass_statistics(text: str) -> bool:
    """Check if the output contains pass statistics (successful compilation)."""
    return "Checkpoint Insertion Statistics" in text


def load_stats_json(data: dict) -> tuple[PassStatistics, bool, str | None]:
    """Load pass statistics from a pre-loaded JSON dict.

    Returns (stats, feasible, infeasibility_reason).
    """

    stats = PassStatistics(
        basic_blocks=data.get("basic_blocks"),
        edges=data.get("edges"),
        regions=data.get("regions"),
        candidate_globals=data.get("candidate_globals"),
        milp_variables=data.get("milp_variables"),
        milp_constraints=data.get("milp_constraints"),
        optimal_solution=data.get("optimal_solution"),
        region_boundaries=data.get("region_boundaries"),
        # MILP writes "boundary_commits_enabled"; maps to distributed_checkpoints
        distributed_checkpoints=data.get("boundary_commits_enabled"),
        solve_time_ms=round(data["solve_time_ms"]) if "solve_time_ms" in data else None,
        compilation_time_ms=round(data["compilation_time_ms"]) if "compilation_time_ms" in data else None,
        peak_rss_kb=data.get("peak_rss_kb"),
        boundary_checks=data.get("boundary_checks"),
        enabled_checkpoints=data.get("enabled_checkpoints"),
        loop_decisions=data.get("loop_decisions"),
        paths_analyzed=data.get("paths_analyzed"),
        runtime_calls_inserted=data.get("runtime_calls_inserted"),
    )

    feasible = data.get("feasible", True)
    infeasibility_reason = data.get("infeasibility_reason")

    return stats, feasible, infeasibility_reason
