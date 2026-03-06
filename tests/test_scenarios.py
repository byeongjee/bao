"""Parametrized pytest tests for all MILP checkpoint insertion scenarios.

Migrated from scenario/run_scenario.sh. Source files live in tests/scenarios/
and configs in tests/scenarios/configs/.
"""

from __future__ import annotations

import pytest

from conftest import SCENARIOS_DIR, CONFIGS_DIR, check_assertions

pytestmark = pytest.mark.milp

# ---------------------------------------------------------------------------
# Scenario table
# Each entry: (scenario_name, source_file, energy_config, milp_config, assertions)
# Config paths are relative to CONFIGS_DIR; source paths relative to SCENARIOS_DIR.
# ---------------------------------------------------------------------------
SCENARIOS = [
    (
        "scenario_no_ckpt",
        "scenario_no_ckpt.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 1, "max_prologue": 1},
    ),
    (
        "scenario_forced_ckpt",
        "scenario_forced_ckpt.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 2, "min_epilogue": 1},
    ),
    (
        "scenario_loop",
        "scenario_loop.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "scenario_nested_loop",
        "scenario_nested_loop.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "scenario_diamond",
        "scenario_diamond.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "scenario_switch",
        "scenario_switch.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "scenario_store_reg",
        "scenario_store_reg.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 2},
    ),
    (
        "scenario_store_global",
        "scenario_store_global.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 2},
    ),
    (
        "scenario_vm_hot",
        "scenario_vm_hot.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "has_shadow": ["g_hot"]},
    ),
    (
        "scenario_needvol",
        "scenario_needvol.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "min_prologue": 2},
    ),
    (
        "scenario_tight",
        "scenario_tight.c",
        "scenario_tight_config.json",
        "scenario_milp_tight_config.json",
        {"exit": 0, "min_prologue": 3},
    ),
    (
        "scenario_infeasible",
        "scenario_infeasible.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0, "stderr_contains": "exceed energy capacity"},
    ),
    (
        "scenario_nvm_efficient",
        "scenario_nvm_efficient.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0},
    ),
    (
        "scenario_noncandidate_ckpt",
        "scenario_noncandidate_ckpt.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0},
    ),
    (
        "scenario_alloca_ckpt",
        "scenario_alloca_ckpt.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0},
    ),
    (
        "scenario_ssa_ckpt",
        "scenario_ssa_ckpt.c",
        "scenario_config.json",
        "scenario_milp_config.json",
        {"exit": 0},
    ),
    (
        "slide_two_phase_checkpoint",
        "slide_basic.c",
        "slide_config.json",
        "slide_milp_config.json",
        {"exit": 0, "min_prologue": 2},
    ),
    (
        "slide_liveout_commit_cost",
        "slide_distributed.c",
        "slide_config.json",
        "slide_milp_config.json",
        {"exit": 0, "min_prologue": 2},
    ),
    (
        "slide_hot_cold_nvm_placement",
        "slide_nvm.c",
        "slide_config.json",
        "slide_milp_nvm_config.json",
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "slide_early_vs_late_boundary",
        "slide_greedy.c",
        "slide_config.json",
        "slide_milp_greedy_config.json",
        {"exit": 0, "min_prologue": 2},
    ),
]

# Extract just the scenario names for pytest IDs
_SCENARIO_IDS = [s[0] for s in SCENARIOS]


@pytest.mark.parametrize(
    "scenario_name,source_file,energy_cfg,milp_cfg,assertions",
    SCENARIOS,
    ids=_SCENARIO_IDS,
)
def test_scenario(
    scenario_name,
    source_file,
    energy_cfg,
    milp_cfg,
    assertions,
    run_milp,
    tmp_path_factory,
):
    tmp_path = tmp_path_factory.mktemp(scenario_name)
    src = SCENARIOS_DIR / source_file
    energy_config = CONFIGS_DIR / energy_cfg
    milp_config = CONFIGS_DIR / milp_cfg

    result = run_milp(src, energy_config, milp_config, tmp_path)
    check_assertions(result, assertions)
