"""Parametrized pytest tests for the SCHEMATIC pass (full trace collection pipeline).

Source files live in tests/scenarios/ and configs in tests/scenarios/configs/.
"""

from __future__ import annotations

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
        {"exit": 0, "min_prologue": 1, "max_prologue": 1},
    ),
    (
        "loop",
        "scenario_loop.c",
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        {"exit": 0, "min_prologue": 1},
    ),
    (
        "switch",
        "scenario_switch.c",
        ENERGY_CONFIG,
        SCHEMATIC_CONFIG,
        {"exit": 0, "min_prologue": 1},
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
