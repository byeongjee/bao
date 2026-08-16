"""Parametrized pytest tests for the SCHEMATIC pass (full trace collection pipeline).

Source files live in tests/scenarios/ and configs in tests/scenarios/configs/.
"""

from __future__ import annotations

import json
import re
import subprocess
from typing import NamedTuple

import pytest
from conftest import (
    CONFIGS_DIR,
    PROJECT_DIR,
    SCENARIOS_DIR,
    PassResult,
    _collect_schematic_trace,
    _prepare_schematic_ir,
    check_assertions,
)

pytestmark = pytest.mark.schematic

ENERGY_CONFIG = CONFIGS_DIR / "scenario_config.json"
SCHEMATIC_CONFIG = CONFIGS_DIR / "scenario_schematic_config.json"
TIGHT_ENERGY_CONFIG = CONFIGS_DIR / "scenario_tight_config.json"

# ---------------------------------------------------------------------------
# Scenario table
# Each entry: (id, source_file, energy_config, schematic_config, assertions)
# ---------------------------------------------------------------------------
SCENARIOS = [
    (
        "no_ckpt",
        "scenario_no_ckpt.c",
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        # Trivial function fits in one region: no boundaries needed.
        {
            "exit": 0,
            "max_boundary": 0,
            "stderr_contains": "Enabled checkpoints:             0",
        },
    ),
    (
        "loop",
        "scenario_loop.c",
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        # Loop fits entirely in one charge (capacity=500, loop costs ~23 energy/iter,
        # 10 iterations = ~230 total — well within budget). Loop analysis runs and
        # determines loopFitsEntirely; no back-edge checkpoint is inserted.
        # The loop body is analyzed as a single region (min_boundary=0 is correct).
        {
            "exit": 0,
            "max_boundary": 0,
            "stderr_contains": "Loop decisions:                  1",
        },
    ),
    (
        "switch",
        "scenario_switch.c",
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        # Multi-path CFG (switch with 4 cases + default). With capacity=500 and
        # IR-level costs (~67 energy for the expensive path), the entire function
        # fits without checkpoints. Validates multi-successor CFG handling: no
        # false infeasibility, correct uncovered-path analysis for untouched cases.
        {"exit": 0, "max_boundary": 0, "stderr_contains": "Paths analyzed:"},
    ),
    (
        "nested_loop_energy",
        "scenario_nested_loop_energy.c",
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        # Inner loop energy far exceeds capacity, so the outer loop must not
        # fit entirely and still needs a back-edge checkpoint. The correct
        # reference-equivalent behavior does NOT blanket-reactivate every
        # fixed edge inside the analyzed loops; the static boundary count
        # stays compact.
        {
            "exit": 0,
            "min_boundary": 3,
            "max_boundary": 3,
            "stderr_contains": "Loop decisions:",
        },
    ),
]

_SCENARIO_IDS = [s[0] for s in SCENARIOS]


def _run_schematic_with_trace(
    tools,
    compile_to_ir,
    src,
    energy_config,
    schematic_config,
    trace_json,
    tmp_path,
    frontend_opt_level,
):
    energy_config = str(energy_config)
    schematic_config = str(schematic_config)
    schematic_input_ll = _prepare_schematic_ir(
        tools, compile_to_ir, src, tmp_path, frontend_opt_level
    )

    output_ll = tmp_path / "output.ll"
    result = subprocess.run(
        [
            tools["opt"],
            "-load-pass-plugin",
            tools["pass_lib"],
            "-passes=schematic",
            f"-energy-config={energy_config}",
            f"-schematic-config={schematic_config}",
            f"-schematic-trace={trace_json}",
            "-S",
            schematic_input_ll,
            "-o",
            str(output_ll),
        ],
        capture_output=True,
        text=True,
        timeout=120,
        check=False,
    )

    output_ir = output_ll.read_text() if output_ll.exists() else ""
    return PassResult(
        exit_code=result.returncode,
        stdout=result.stdout,
        stderr=result.stderr,
        output_ir=output_ir,
    )


def _run_ckpt_compile_schematic_o3(
    benchmark_name,
    cap_value,
    output_prefix,
):
    result = subprocess.run(
        [
            "uv",
            "run",
            "ckpt",
            "--log-level",
            "debug",
            "compile",
            "schematicO3",
            benchmark_name,
            "--cap",
            cap_value,
            "-o",
            str(output_prefix),
        ],
        capture_output=True,
        text=True,
        cwd=str(PROJECT_DIR),
        timeout=240,
        check=False,
    )
    return result, result.stdout + result.stderr


class LoopBudget(NamedTuple):
    # E_loop is logged before refineLoopBudgetWithConvergence and numIt after,
    # so the two are not a single snapshot.
    e_loop: float
    num_it: int
    max_trip_count: int


_NUMBER = r"[0-9.eE+-]+"


def _extract_loop_budget(
    log_text,
    loop_header,
):
    e_loop_match = re.search(
        rf"\[LoopAnalyzer\]\s+loop={re.escape(loop_header)}\s+E_loop=({_NUMBER})",
        log_text,
    )
    num_it_match = re.search(
        rf"\[LoopAnalyzer\]\s+loop={re.escape(loop_header)}\s+numIt=(\d+)"
        rf"\s+maxTripCount=(\d+)",
        log_text,
    )
    assert e_loop_match, f"Missing E_loop log for {loop_header}:\n{log_text[-4000:]}"
    assert num_it_match, f"Missing numIt log for {loop_header}:\n{log_text[-4000:]}"
    return LoopBudget(
        e_loop=float(e_loop_match.group(1)),
        num_it=int(num_it_match.group(1)),
        max_trip_count=int(num_it_match.group(2)),
    )


def _write_trace(trace, trace_json):
    trace_json.write_text(json.dumps(trace, indent=2))


@pytest.mark.parametrize(
    "scenario_id,source_file,energy_cfg,schematic_cfg,assertions",
    SCENARIOS,
    ids=_SCENARIO_IDS,
)
def test_schematic(
    scenario_id,
    source_file,
    energy_cfg,
    schematic_cfg,
    assertions,
    run_schematic,
    tmp_path_factory,
):
    tmp_path = tmp_path_factory.mktemp(scenario_id)
    src = SCENARIOS_DIR / source_file

    result = run_schematic(src, energy_cfg, schematic_cfg, tmp_path)
    check_assertions(result, assertions)


def test_schematic_alloca_placement(run_schematic, tmp_path_factory):
    """Test that O0 allocas become placement candidates in SCHEMATIC.

    At O0, local variables are alloca+load/store pairs. With
    SchematicStateAnalysis, these allocas are eligible for VM/NVM placement.
    VM-placed allocas get __vm_shadow_ globals; NVM-placed allocas don't.
    """
    tmp_path = tmp_path_factory.mktemp("alloca_placement")
    src = SCENARIOS_DIR / "scenario_alloca_placement.c"

    result = run_schematic(src, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path)

    # Must succeed.
    assert result.exit_code == 0, (
        f"Expected exit=0 but got {result.exit_code}.\nstderr: {result.stderr[:1000]}"
    )

    # The __region_boundary function must be declared (even if not called,
    # since this trivial function may fit in a single region with 0 boundaries).
    assert "__region_boundary" in result.output_ir, (
        "Expected __region_boundary to be declared in the output IR"
    )

    # The function has local variables compiled at O0 (alloca-based).
    # Check that the pass found candidates (globals + allocas).
    assert "Candidate globals (V_elig):" in result.stderr
    # Extract candidate count — should be > 0 (at least g_result + allocas).
    m = re.search(r"Candidate globals \(V_elig\):\s+(\d+)", result.stderr)
    assert m, "Could not find candidate count in stderr"
    candidate_count = int(m.group(1))
    assert candidate_count >= 1, (
        f"Expected >= 1 candidates (globals + allocas), got {candidate_count}"
    )


def test_schematic_trace_function_path_excludes_nested_loop_blocks(
    collect_schematic_trace,
    tmp_path_factory,
):
    """Function traces should not absorb inner-loop blocks from nested loops.

    This matches the reference trace manager semantics: inner-loop iterations
    are recorded in loop traces, while the function trace continues directly to
    the post-loop block.
    """
    tmp_path = tmp_path_factory.mktemp("nested_loop_trace")
    src = SCENARIOS_DIR / "scenario_nested_loop_energy.c"

    trace = collect_schematic_trace(src, ENERGY_CONFIG, tmp_path, 0)
    function_path = trace["main"]["traces"][0]["path"]

    assert "for.cond1" not in function_path
    assert "for.body3" not in function_path


def test_schematic_o3_nested_loop_energy(run_schematic_o3, tmp_path_factory):
    """Optimized nested loops must still charge the parent loop for child work."""
    tmp_path = tmp_path_factory.mktemp("nested_loop_energy_o3")
    src = SCENARIOS_DIR / "scenario_nested_loop_energy_o3.c"

    result = run_schematic_o3(src, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path)

    assert result.exit_code == 0, (
        f"Expected exit=0 but got {result.exit_code}.\nstderr: {result.stderr[:1000]}"
    )
    assert "Loop decisions:                  2" in result.stderr

    m = re.search(r"Region boundaries:\s+(\d+)", result.stderr)
    assert m, f"Could not find region boundary count in stderr:\n{result.stderr}"
    assert int(m.group(1)) >= 4, result.stderr


def test_schematic_o3_dijkstra_loop_budget_uses_rare_inner_branch(
    tmp_path_factory,
):
    tmp_path = tmp_path_factory.mktemp("schematic_dijkstra_o3")

    result, log_text = _run_ckpt_compile_schematic_o3(
        "dijkstra",
        "10uF",
        tmp_path / "dijkstra_schematicO3",
    )

    assert result.returncode == 0, log_text[-4000:]

    inner = _extract_loop_budget(log_text, "for.body18.i")
    outer = _extract_loop_budget(log_text, "while.body.i")

    # The inner loop is entered rarely in the trace, but a single outer
    # iteration may still run it to its full trip count. The outer budget must
    # therefore cover the whole inner loop, not just the hot path through it.
    inner_worst_case = inner.e_loop * inner.max_trip_count
    assert outer.e_loop >= inner_worst_case, (
        f"outer E_loop {outer.e_loop} ignores the rare inner branch "
        f"(worst-case inner cost {inner_worst_case})\n{log_text[-4000:]}"
    )

    assert outer.num_it < outer.max_trip_count, log_text[-4000:]


def test_schematic_synthesizes_missing_top_level_loop_trace(
    collect_schematic_trace,
    tools,
    compile_to_ir,
    tmp_path_factory,
):
    tmp_path = tmp_path_factory.mktemp("schematic_missing_top_level_loop_trace")
    src = SCENARIOS_DIR / "scenario_loop.c"

    trace = collect_schematic_trace(src, ENERGY_CONFIG, tmp_path, 0)
    assert len(trace["main"]["loop_traces"]) == 1, trace

    trace["main"]["loop_traces"] = {}
    trace_json = tmp_path / "missing_top_level_loop_trace.json"
    _write_trace(trace, trace_json)

    result = _run_schematic_with_trace(
        tools,
        compile_to_ir,
        src,
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        trace_json,
        tmp_path,
        0,
    )

    assert result.exit_code == 0, result.stderr[:1000]
    assert "Loop decisions:                  1" in result.stderr


def test_schematic_synthesizes_missing_nested_loop_trace(
    collect_schematic_trace,
    tools,
    compile_to_ir,
    tmp_path_factory,
):
    tmp_path = tmp_path_factory.mktemp("schematic_missing_nested_loop_trace")
    src = SCENARIOS_DIR / "scenario_nested_loop_energy.c"

    trace = collect_schematic_trace(src, ENERGY_CONFIG, tmp_path, 0)
    loop_traces = trace["main"]["loop_traces"]
    assert len(loop_traces) == 2, loop_traces

    deepest_header = max(
        loop_traces.items(), key=lambda item: item[1]["loop"]["depth"]
    )[0]
    del loop_traces[deepest_header]

    trace_json = tmp_path / "missing_nested_loop_trace.json"
    _write_trace(trace, trace_json)

    result = _run_schematic_with_trace(
        tools,
        compile_to_ir,
        src,
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        trace_json,
        tmp_path,
        0,
    )

    assert result.exit_code == 0, result.stderr[:1000]
    assert "Loop decisions:                  2" in result.stderr


def test_schematic_debug_loop_logs_respect_log_level(
    tools, compile_to_ir, tmp_path_factory
):
    tmp_path = tmp_path_factory.mktemp("schematic_log_levels")
    src = SCENARIOS_DIR / "scenario_nested_loop_energy_o3.c"

    schematic_input_ll = _prepare_schematic_ir(tools, compile_to_ir, src, tmp_path, 3)
    trace_json = _collect_schematic_trace(
        tools, schematic_input_ll, ENERGY_CONFIG, tmp_path
    )

    def run_with_log_level(log_level: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                tools["opt"],
                "-load-pass-plugin",
                tools["pass_lib"],
                "-passes=schematic",
                f"-energy-config={ENERGY_CONFIG}",
                f"-schematic-config={SCHEMATIC_CONFIG}",
                f"-schematic-trace={trace_json}",
                f"-ckpt-log-level={log_level}",
                "-S",
                schematic_input_ll,
                "-o",
                str(tmp_path / f"output_{log_level}.ll"),
            ],
            capture_output=True,
            text=True,
            timeout=120,
            check=False,
        )

    info_result = run_with_log_level("info")
    debug_result = run_with_log_level("debug")

    assert info_result.returncode == 0, info_result.stderr
    assert debug_result.returncode == 0, debug_result.stderr

    assert "[LoopAnalyzer] loop=" not in info_result.stderr
    assert "[DEBUG RCG]" not in info_result.stderr
    assert "[LoopAnalyzer] loop=" in debug_result.stderr
    assert "[DEBUG RCG]" in debug_result.stderr


def test_schematic_skips_debug_helper_functions(
    collect_schematic_trace,
    run_schematic,
    tmp_path_factory,
):
    tmp_path = tmp_path_factory.mktemp("schematic_skips_debug_helpers")
    src = SCENARIOS_DIR / "scenario_schematic_skips_debug_helpers.c"

    trace = collect_schematic_trace(src, ENERGY_CONFIG, tmp_path, 0)
    assert sorted(trace.keys()) == ["main"]

    result = run_schematic(src, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path)
    assert result.exit_code == 0, (
        f"Expected exit=0 but got {result.exit_code}.\nstderr: {result.stderr[:1000]}"
    )
    assert (
        "SCHEMATIC: skipping benchmark infrastructure function timing_gpio_start"
        in result.stderr
    )
    assert (
        "SCHEMATIC: skipping benchmark infrastructure function timing_gpio_stop"
        in result.stderr
    )
    assert (
        "SCHEMATIC: skipping benchmark infrastructure function _timing_delay_cycles"
        in result.stderr
    )
    assert "due to unresolved memory/call effects" not in result.stderr
