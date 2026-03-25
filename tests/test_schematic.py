"""Parametrized pytest tests for the SCHEMATIC pass (full trace collection pipeline).

Source files live in tests/scenarios/ and configs in tests/scenarios/configs/.
"""

from __future__ import annotations

import re

import pytest

from conftest import SCENARIOS_DIR, CONFIGS_DIR, check_assertions

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
        {"exit": 0, "max_boundary": 0,
         "stderr_contains": "Enabled checkpoints:             0"},
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
        {"exit": 0, "max_boundary": 0,
         "stderr_contains": "Loop decisions:                  1"},
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
        {"exit": 0, "max_boundary": 0,
         "stderr_contains": "Paths analyzed:"},
    ),
    (
        "nested_loop_energy",
        "scenario_nested_loop_energy.c",
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        # Inner loop energy far exceeds capacity — outer loop must NOT fit
        # entirely. With the nested-energy-propagation fix, the outer loop
        # back-edge also gets a checkpoint (mandatoryBackEdge). Both loops
        # must produce checkpoints (min 2 loop decisions, 3+ enabled ckpts).
        {"exit": 0, "min_boundary": 4,
         "stderr_contains": "Loop decisions:"},
    ),
]

_SCENARIO_IDS = [s[0] for s in SCENARIOS]


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


def test_schematic_alloca_placement(run_schematic_o0, tmp_path_factory):
    """Test that O0 allocas become placement candidates in SCHEMATIC.

    At O0, local variables are alloca+load/store pairs. With
    SchematicStateAnalysis, these allocas are eligible for VM/NVM placement.
    VM-placed allocas get __vm_shadow_ globals; NVM-placed allocas don't.
    """
    import re

    tmp_path = tmp_path_factory.mktemp("alloca_placement")
    src = SCENARIOS_DIR / "scenario_alloca_placement.c"

    result = run_schematic_o0(src, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path)

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


def test_schematic_skips_debug_helper_functions(
    collect_schematic_trace,
    run_schematic_o0,
    tmp_path_factory,
):
    tmp_path = tmp_path_factory.mktemp("schematic_skips_debug_helpers")
    src = SCENARIOS_DIR / "scenario_schematic_skips_debug_helpers.c"

    trace = collect_schematic_trace(src, ENERGY_CONFIG, tmp_path, 0)
    assert sorted(trace.keys()) == ["main"]

    result = run_schematic_o0(src, ENERGY_CONFIG, SCHEMATIC_CONFIG, tmp_path)
    assert result.exit_code == 0, (
        f"Expected exit=0 but got {result.exit_code}.\nstderr: {result.stderr[:1000]}"
    )
    assert "SCHEMATIC: skipping benchmark infrastructure function timing_gpio_start" in result.stderr
    assert "SCHEMATIC: skipping benchmark infrastructure function timing_gpio_stop" in result.stderr
    assert "SCHEMATIC: skipping benchmark infrastructure function _timing_delay_cycles" in result.stderr
    assert "due to unresolved memory/call effects" not in result.stderr
